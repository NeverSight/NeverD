//===- MachOSignaturePolicyTests.cpp - Mach-O signing policy tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/MachO/MachOSignaturePolicy.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd::macho_signature;

constexpr uint32_t DerEntitlementsSlot = 7;
constexpr uint32_t DerEntitlementsMagic = 0xfade7172;

struct Slot {
  uint32_t Type = 0;
  std::vector<uint8_t> Blob;
};

void writeBE32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write32be(Bytes.data() + Offset, Value);
}

void writeBE64(std::vector<uint8_t> &Bytes, size_t Offset, uint64_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write64be(Bytes.data() + Offset, Value);
}

std::vector<uint8_t>
codeDirectory(uint32_t Flags, std::string Identifier,
              uint32_t CodeLimit = sizeof(llvm::MachO::mach_header_64) +
                                   sizeof(llvm::MachO::linkedit_data_command)) {
  using namespace llvm::MachO;
  constexpr uint8_t PageSize = 12;
  constexpr uint32_t CodeSlots = 1;
  const size_t IdentifierOffset = sizeof(CS_CodeDirectory);
  const size_t HashOffset = IdentifierOffset + Identifier.size() + 1;
  std::vector<uint8_t> Bytes(HashOffset + CS_SHA256_LEN * CodeSlots, 0);
  writeBE32(Bytes, offsetof(CS_CodeDirectory, magic), CSMAGIC_CODEDIRECTORY);
  writeBE32(Bytes, offsetof(CS_CodeDirectory, length),
            static_cast<uint32_t>(Bytes.size()));
  writeBE32(Bytes, offsetof(CS_CodeDirectory, version), CS_SUPPORTSEXECSEG);
  writeBE32(Bytes, offsetof(CS_CodeDirectory, flags), Flags);
  writeBE32(Bytes, offsetof(CS_CodeDirectory, identOffset),
            static_cast<uint32_t>(IdentifierOffset));
  writeBE32(Bytes, offsetof(CS_CodeDirectory, hashOffset),
            static_cast<uint32_t>(HashOffset));
  writeBE32(Bytes, offsetof(CS_CodeDirectory, nCodeSlots), CodeSlots);
  writeBE32(Bytes, offsetof(CS_CodeDirectory, codeLimit), CodeLimit);
  Bytes[offsetof(CS_CodeDirectory, hashSize)] = CS_SHA256_LEN;
  Bytes[offsetof(CS_CodeDirectory, hashType)] = CS_HASHTYPE_SHA256;
  Bytes[offsetof(CS_CodeDirectory, pageSize)] = PageSize;
  writeBE64(Bytes, offsetof(CS_CodeDirectory, execSegLimit), CodeLimit);
  writeBE64(Bytes, offsetof(CS_CodeDirectory, execSegFlags),
            CS_EXECSEG_MAIN_BINARY);
  std::memcpy(Bytes.data() + IdentifierOffset, Identifier.data(),
              Identifier.size());
  return Bytes;
}

std::vector<uint8_t> genericBlob(uint32_t Magic) {
  using namespace llvm::MachO;
  std::vector<uint8_t> Bytes(offsetof(CS_CodeDirectory, version), 0);
  writeBE32(Bytes, offsetof(CS_CodeDirectory, magic), Magic);
  writeBE32(Bytes, offsetof(CS_CodeDirectory, length),
            static_cast<uint32_t>(Bytes.size()));
  return Bytes;
}

std::vector<uint8_t> emptyRequirementsBlob() {
  using namespace llvm::MachO;
  std::vector<uint8_t> Bytes(sizeof(CS_SuperBlob), 0);
  writeBE32(Bytes, offsetof(CS_SuperBlob, magic), CSMAGIC_REQUIREMENTS);
  writeBE32(Bytes, offsetof(CS_SuperBlob, length),
            static_cast<uint32_t>(Bytes.size()));
  writeBE32(Bytes, offsetof(CS_SuperBlob, count), 0);
  return Bytes;
}

std::vector<uint8_t>
codesignOutputCodeDirectory(uint32_t Flags, std::string Identifier,
                            llvm::ArrayRef<uint8_t> Requirements) {
  using namespace llvm::MachO;
  std::vector<uint8_t> Directory = codeDirectory(Flags, std::move(Identifier));
  const uint32_t OldHashOffset = llvm::support::endian::read32be(
      Directory.data() + offsetof(CS_CodeDirectory, hashOffset));
  Directory.insert(Directory.begin() + OldHashOffset, 2 * CS_SHA256_LEN, 0);
  writeBE32(Directory, offsetof(CS_CodeDirectory, length),
            static_cast<uint32_t>(Directory.size()));
  writeBE32(Directory, offsetof(CS_CodeDirectory, hashOffset),
            OldHashOffset + 2 * CS_SHA256_LEN);
  writeBE32(Directory, offsetof(CS_CodeDirectory, nSpecialSlots), 2);
  const std::array<uint8_t, CS_SHA256_LEN> RequirementsHash =
      llvm::SHA256::hash(Requirements);
  std::copy(RequirementsHash.begin(), RequirementsHash.end(),
            Directory.begin() + OldHashOffset);
  return Directory;
}

std::vector<Slot>
canonicalCodesignOutputSlots(std::string Identifier = "com.example.tool",
                             uint32_t Flags = llvm::MachO::CS_ADHOC) {
  using namespace llvm::MachO;
  std::vector<uint8_t> Requirements = emptyRequirementsBlob();
  std::vector<uint8_t> Directory =
      codesignOutputCodeDirectory(Flags, std::move(Identifier), Requirements);
  return {{CSSLOT_CODEDIRECTORY, std::move(Directory)},
          {CSSLOT_REQUIREMENTS, std::move(Requirements)},
          {CSSLOT_SIGNATURESLOT, genericBlob(CSMAGIC_BLOBWRAPPER)}};
}

std::vector<uint8_t> superBlob(std::vector<Slot> Slots) {
  using namespace llvm::MachO;
  const size_t IndexBytes = Slots.size() * sizeof(CS_BlobIndex);
  const size_t HeaderBytes = sizeof(CS_SuperBlob) + IndexBytes;
  size_t Total = HeaderBytes;
  for (const Slot &Entry : Slots)
    Total += Entry.Blob.size();
  std::vector<uint8_t> Bytes(Total, 0);
  writeBE32(Bytes, offsetof(CS_SuperBlob, magic), CSMAGIC_EMBEDDED_SIGNATURE);
  writeBE32(Bytes, offsetof(CS_SuperBlob, length),
            static_cast<uint32_t>(Bytes.size()));
  writeBE32(Bytes, offsetof(CS_SuperBlob, count),
            static_cast<uint32_t>(Slots.size()));

  size_t BlobOffset = HeaderBytes;
  for (size_t Index = 0; Index < Slots.size(); ++Index) {
    const size_t EntryOffset =
        sizeof(CS_SuperBlob) + Index * sizeof(CS_BlobIndex);
    writeBE32(Bytes, EntryOffset + offsetof(CS_BlobIndex, type),
              Slots[Index].Type);
    writeBE32(Bytes, EntryOffset + offsetof(CS_BlobIndex, offset),
              static_cast<uint32_t>(BlobOffset));
    std::copy(Slots[Index].Blob.begin(), Slots[Index].Blob.end(),
              Bytes.begin() + BlobOffset);
    BlobOffset += Slots[Index].Blob.size();
  }
  return Bytes;
}

std::vector<uint8_t> thinMachO(std::vector<uint8_t> Signature = {},
                               uint32_t CPUType = llvm::MachO::CPU_TYPE_X86_64,
                               unsigned SignatureCommands = 1) {
  using namespace llvm::MachO;
  if (Signature.empty())
    SignatureCommands = 0;
  const size_t CommandsSize = SignatureCommands * sizeof(linkedit_data_command);
  const size_t SignatureOffset = sizeof(mach_header_64) + CommandsSize;
  std::vector<uint8_t> Bytes(SignatureOffset + Signature.size(), 0);
  mach_header_64 Header{};
  Header.magic = MH_MAGIC_64;
  Header.cputype = static_cast<int32_t>(CPUType);
  Header.cpusubtype = CPU_SUBTYPE_MULTIPLE;
  Header.filetype = MH_EXECUTE;
  Header.ncmds = SignatureCommands;
  Header.sizeofcmds = static_cast<uint32_t>(CommandsSize);
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  for (unsigned Index = 0; Index < SignatureCommands; ++Index) {
    linkedit_data_command Command{};
    Command.cmd = LC_CODE_SIGNATURE;
    Command.cmdsize = sizeof(linkedit_data_command);
    Command.dataoff = static_cast<uint32_t>(SignatureOffset);
    Command.datasize = static_cast<uint32_t>(Signature.size());
    std::memcpy(Bytes.data() + sizeof(mach_header_64) +
                    Index * sizeof(linkedit_data_command),
                &Command, sizeof(Command));
  }
  std::copy(Signature.begin(), Signature.end(),
            Bytes.begin() + SignatureOffset);
  return Bytes;
}

std::vector<uint8_t> thinMachOWithExtendedSignatureCommand() {
  using namespace llvm::MachO;
  constexpr uint32_t ExtendedCommandSize = sizeof(linkedit_data_command) + 8;
  constexpr uint32_t SignatureOffset =
      sizeof(mach_header_64) + ExtendedCommandSize;
  std::vector<uint8_t> Signature = superBlob(
      {{CSSLOT_CODEDIRECTORY,
        codeDirectory(CS_ADHOC, "com.example.tool", SignatureOffset)}});
  std::vector<uint8_t> Bytes(SignatureOffset + Signature.size(), 0);
  mach_header_64 Header{};
  Header.magic = MH_MAGIC_64;
  Header.cputype = CPU_TYPE_X86_64;
  Header.cpusubtype = CPU_SUBTYPE_MULTIPLE;
  Header.filetype = MH_EXECUTE;
  Header.ncmds = 1;
  Header.sizeofcmds = ExtendedCommandSize;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  linkedit_data_command Command{};
  Command.cmd = LC_CODE_SIGNATURE;
  Command.cmdsize = ExtendedCommandSize;
  Command.dataoff = SignatureOffset;
  Command.datasize = static_cast<uint32_t>(Signature.size());
  std::memcpy(Bytes.data() + sizeof(Header), &Command, sizeof(Command));
  std::copy(Signature.begin(), Signature.end(),
            Bytes.begin() + SignatureOffset);
  return Bytes;
}

std::vector<uint8_t> fatMachO(std::vector<uint8_t> Left,
                              std::vector<uint8_t> Right) {
  using namespace llvm::MachO;
  constexpr size_t SliceAlignment = 0x1000;
  const size_t TableSize = sizeof(fat_header) + 2 * sizeof(fat_arch);
  const size_t LeftOffset =
      (TableSize + SliceAlignment - 1) & ~(SliceAlignment - 1);
  const size_t RightOffset =
      (LeftOffset + Left.size() + SliceAlignment - 1) & ~(SliceAlignment - 1);
  std::vector<uint8_t> Bytes(RightOffset + Right.size(), 0);
  writeBE32(Bytes, offsetof(fat_header, magic), FAT_MAGIC);
  writeBE32(Bytes, offsetof(fat_header, nfat_arch), 2);

  const size_t LeftArch = sizeof(fat_header);
  writeBE32(Bytes, LeftArch + offsetof(fat_arch, cputype), CPU_TYPE_X86_64);
  writeBE32(Bytes, LeftArch + offsetof(fat_arch, cpusubtype),
            CPU_SUBTYPE_MULTIPLE);
  writeBE32(Bytes, LeftArch + offsetof(fat_arch, offset),
            static_cast<uint32_t>(LeftOffset));
  writeBE32(Bytes, LeftArch + offsetof(fat_arch, size),
            static_cast<uint32_t>(Left.size()));
  writeBE32(Bytes, LeftArch + offsetof(fat_arch, align), 12);

  const size_t RightArch = LeftArch + sizeof(fat_arch);
  writeBE32(Bytes, RightArch + offsetof(fat_arch, cputype), CPU_TYPE_ARM64);
  writeBE32(Bytes, RightArch + offsetof(fat_arch, cpusubtype),
            CPU_SUBTYPE_MULTIPLE);
  writeBE32(Bytes, RightArch + offsetof(fat_arch, offset),
            static_cast<uint32_t>(RightOffset));
  writeBE32(Bytes, RightArch + offsetof(fat_arch, size),
            static_cast<uint32_t>(Right.size()));
  writeBE32(Bytes, RightArch + offsetof(fat_arch, align), 12);

  std::copy(Left.begin(), Left.end(), Bytes.begin() + LeftOffset);
  std::copy(Right.begin(), Right.end(), Bytes.begin() + RightOffset);
  return Bytes;
}

llvm::Expected<Profile> inspectBytes(const std::vector<uint8_t> &Bytes) {
  return inspect(llvm::ArrayRef<uint8_t>(Bytes));
}

std::vector<uint8_t> withCodeDirectoryHash(uint32_t Flags,
                                           std::string Identifier,
                                           uint8_t HashType, uint8_t HashSize,
                                           uint32_t CodeSlots) {
  using namespace llvm::MachO;
  std::vector<uint8_t> Directory = codeDirectory(Flags, std::move(Identifier));
  const uint32_t HashOffset = llvm::support::endian::read32be(
      Directory.data() + offsetof(CS_CodeDirectory, hashOffset));
  Directory.resize(HashOffset + uint64_t(HashSize) * CodeSlots);
  writeBE32(Directory, offsetof(CS_CodeDirectory, length),
            static_cast<uint32_t>(Directory.size()));
  writeBE32(Directory, offsetof(CS_CodeDirectory, nCodeSlots), CodeSlots);
  Directory[offsetof(CS_CodeDirectory, hashType)] = HashType;
  Directory[offsetof(CS_CodeDirectory, hashSize)] = HashSize;
  return Directory;
}

std::string takeError(llvm::Expected<Profile> &Result) {
  return llvm::toString(Result.takeError());
}

TEST(MachOSignaturePolicy, UnsignedThinImageIsNotResignEligible) {
  llvm::Expected<Profile> Result = inspectBytes(thinMachO());

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Slices.size(), 1u);
  EXPECT_EQ(Result->Slices[0].SignatureKind, Kind::Unsigned);
  EXPECT_FALSE(canTransactionallyAdHocResign(*Result));
}

TEST(MachOSignaturePolicy, LinkerAndPlainAdHocSignaturesAreEligible) {
  using namespace llvm::MachO;
  for (uint32_t Flags :
       {uint32_t(CS_ADHOC), uint32_t(CS_ADHOC | CS_LINKER_SIGNED)}) {
    SCOPED_TRACE(Flags);
    llvm::Expected<Profile> Result = inspectBytes(thinMachO(superBlob(
        {{CSSLOT_CODEDIRECTORY, codeDirectory(Flags, "com.example.tool")}})));

    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Slices.size(), 1u);
    const SliceProfile &Slice = Result->Slices[0];
    EXPECT_EQ(Slice.SignatureKind, Kind::IdentitylessAdHoc);
    EXPECT_EQ(Slice.Identifier, "com.example.tool");
    EXPECT_EQ(Slice.LinkerSigned, (Flags & CS_LINKER_SIGNED) != 0);
    EXPECT_FALSE(Slice.HasUnsupportedFlags);
    EXPECT_FALSE(Slice.HasUnsupportedSlots);
    EXPECT_TRUE(canTransactionallyAdHocResign(*Result));
  }
}

TEST(MachOSignaturePolicy, RealLinkerAdHocFixtureIsEligible) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buffer =
      llvm::MemoryBuffer::getFile(NEVERD_MACHO_SIGNATURE_FIXTURE, false);
  ASSERT_TRUE(Buffer) << Buffer.getError().message();
  const llvm::StringRef Contents = (*Buffer)->getBuffer();
  llvm::Expected<Profile> Result = inspect(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Contents.data()), Contents.size()));

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Slices.size(), 1u);
  EXPECT_EQ(Result->Slices[0].SignatureKind, Kind::IdentitylessAdHoc);
  EXPECT_EQ(Result->Slices[0].Identifier, "safety_cases_macho_arm64");
  EXPECT_TRUE(Result->Slices[0].LinkerSigned);
  EXPECT_TRUE(canTransactionallyAdHocResign(*Result));
}

TEST(MachOSignaturePolicy, ZeroAlignmentPaddingIsAccepted) {
  using namespace llvm::MachO;
  std::vector<uint8_t> Signature = superBlob(
      {{CSSLOT_CODEDIRECTORY, codeDirectory(CS_ADHOC, "com.example.tool")}});
  Signature.resize(Signature.size() + 7, 0);

  llvm::Expected<Profile> Result = inspectBytes(thinMachO(Signature));

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE(canTransactionallyAdHocResign(*Result));
}

TEST(MachOSignaturePolicy, EntitlementsFailClosed) {
  using namespace llvm::MachO;
  const Slot Primary{CSSLOT_CODEDIRECTORY,
                     codeDirectory(CS_ADHOC, "com.example.tool")};
  for (Slot Entitlements :
       {Slot{CSSLOT_ENTITLEMENTS, genericBlob(CSMAGIC_EMBEDDED_ENTITLEMENTS)},
        Slot{DerEntitlementsSlot, genericBlob(DerEntitlementsMagic)}}) {
    llvm::Expected<Profile> Result =
        inspectBytes(thinMachO(superBlob({Primary, Entitlements})));

    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Slices.size(), 1u);
    EXPECT_EQ(Result->Slices[0].SignatureKind, Kind::Entitled);
    EXPECT_FALSE(canTransactionallyAdHocResign(*Result));
  }
}

TEST(MachOSignaturePolicy, HardenedRuntimeFailsClosed) {
  using namespace llvm::MachO;
  llvm::Expected<Profile> Result = inspectBytes(thinMachO(
      superBlob({{CSSLOT_CODEDIRECTORY,
                  codeDirectory(CS_ADHOC | CS_RUNTIME, "com.example.tool")}})));

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Slices[0].SignatureKind, Kind::Hardened);
  EXPECT_FALSE(canTransactionallyAdHocResign(*Result));
}

TEST(MachOSignaturePolicy,
     RuntimeVersionCodeDirectoryIsParsedButNeverResignEligible) {
  using namespace llvm::MachO;
  const auto RuntimeDirectory = [](uint32_t Flags) {
    std::vector<uint8_t> Directory = codeDirectory(Flags, "com.example.tool");
    const uint32_t IdentifierOffset = llvm::support::endian::read32be(
        Directory.data() + offsetof(CS_CodeDirectory, identOffset));
    const uint32_t HashOffset = llvm::support::endian::read32be(
        Directory.data() + offsetof(CS_CodeDirectory, hashOffset));
    Directory.insert(Directory.begin() + IdentifierOffset, 8, 0);
    writeBE32(Directory, offsetof(CS_CodeDirectory, length),
              static_cast<uint32_t>(Directory.size()));
    writeBE32(Directory, offsetof(CS_CodeDirectory, version),
              CS_SUPPORTSRUNTIME);
    writeBE32(Directory, offsetof(CS_CodeDirectory, identOffset),
              IdentifierOffset + 8);
    writeBE32(Directory, offsetof(CS_CodeDirectory, hashOffset),
              HashOffset + 8);
    writeBE32(Directory, sizeof(CS_CodeDirectory), 0x000d0000);
    return Directory;
  };

  for (uint32_t Flags : {uint32_t(CS_ADHOC | CS_RUNTIME), uint32_t(CS_ADHOC)}) {
    SCOPED_TRACE(Flags);
    llvm::Expected<Profile> Result = inspectBytes(thinMachO(
        superBlob({{CSSLOT_CODEDIRECTORY, RuntimeDirectory(Flags)}})));

    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Slices[0].CodeDirectoryVersion, CS_SUPPORTSRUNTIME);
    EXPECT_EQ(Result->Slices[0].SignatureKind, (Flags & CS_RUNTIME) != 0
                                                   ? Kind::Hardened
                                                   : Kind::IdentitylessAdHoc);
    EXPECT_FALSE(canTransactionallyAdHocResign(*Result));
  }
}

TEST(MachOSignaturePolicy, DeveloperCMSSignatureFailsClosed) {
  using namespace llvm::MachO;
  llvm::Expected<Profile> Result = inspectBytes(thinMachO(
      superBlob({{CSSLOT_CODEDIRECTORY, codeDirectory(0, "com.example.tool")},
                 {CSSLOT_SIGNATURESLOT, genericBlob(CSMAGIC_BLOBWRAPPER)}})));

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Slices[0].SignatureKind, Kind::DeveloperSigned);
  EXPECT_FALSE(canTransactionallyAdHocResign(*Result));
}

TEST(MachOSignaturePolicy, UnknownOrRequirementsSlotsFailClosed) {
  using namespace llvm::MachO;
  const Slot Primary{CSSLOT_CODEDIRECTORY,
                     codeDirectory(CS_ADHOC, "com.example.tool")};
  for (Slot Strong : {Slot{CSSLOT_REQUIREMENTS, emptyRequirementsBlob()},
                      Slot{0x8000, genericBlob(0xfade0c01)}}) {
    SCOPED_TRACE(Strong.Type);
    llvm::Expected<Profile> Result =
        inspectBytes(thinMachO(superBlob({Primary, std::move(Strong)})));

    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    EXPECT_TRUE(Result->Slices[0].HasUnsupportedSlots);
    EXPECT_FALSE(canTransactionallyAdHocResign(*Result));
  }
}

TEST(MachOSignaturePolicy, UnknownCodeDirectoryFlagsFailClosed) {
  using namespace llvm::MachO;
  llvm::Expected<Profile> Result = inspectBytes(thinMachO(superBlob(
      {{CSSLOT_CODEDIRECTORY,
        codeDirectory(CS_ADHOC | CS_EXEC_INHERIT_SIP, "com.example.tool")}})));

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE(Result->Slices[0].HasUnsupportedFlags);
  EXPECT_FALSE(canTransactionallyAdHocResign(*Result));
}

TEST(MachOSignaturePolicy, StrongAlternateCodeDirectoryWins) {
  using namespace llvm::MachO;
  llvm::Expected<Profile> Result = inspectBytes(thinMachO(superBlob(
      {{CSSLOT_CODEDIRECTORY, codeDirectory(CS_ADHOC, "com.example.tool")},
       {CSSLOT_ALTERNATE_CODEDIRECTORIES,
        codeDirectory(CS_ADHOC | CS_RUNTIME, "com.example.tool")}})));

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Slices[0].SignatureKind, Kind::Hardened);
  EXPECT_FALSE(canTransactionallyAdHocResign(*Result));
}

TEST(MachOSignaturePolicy, EvenEquivalentAlternateCodeDirectoryFailsClosed) {
  using namespace llvm::MachO;
  llvm::Expected<Profile> Result = inspectBytes(thinMachO(superBlob(
      {{CSSLOT_CODEDIRECTORY, codeDirectory(CS_ADHOC, "com.example.tool")},
       {CSSLOT_ALTERNATE_CODEDIRECTORIES,
        codeDirectory(CS_ADHOC, "com.example.tool")}})));

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE(Result->Slices[0].HasUnsupportedSlots);
  EXPECT_FALSE(canTransactionallyAdHocResign(*Result));
}

TEST(MachOSignaturePolicy, CodeDirectoryBoundsAreValidated) {
  using namespace llvm::MachO;
  const std::vector<uint8_t> Valid =
      codeDirectory(CS_ADHOC, "com.example.tool");
  std::vector<std::pair<std::string, std::vector<uint8_t>>> Cases;

  std::vector<uint8_t> ShortVersion = Valid;
  writeBE32(ShortVersion, offsetof(CS_CodeDirectory, length),
            offsetof(CS_CodeDirectory, execSegBase));
  ShortVersion.resize(offsetof(CS_CodeDirectory, execSegBase));
  Cases.emplace_back("versioned header", ShortVersion);

  std::vector<uint8_t> FutureVersion = Valid;
  writeBE32(FutureVersion, offsetof(CS_CodeDirectory, version),
            CS_SUPPORTSLINKAGE);
  Cases.emplace_back("version is unsupported", FutureVersion);

  std::vector<uint8_t> BadHashOffset = Valid;
  writeBE32(BadHashOffset, offsetof(CS_CodeDirectory, hashOffset),
            static_cast<uint32_t>(BadHashOffset.size() + 1));
  Cases.emplace_back("hash region", BadHashOffset);

  std::vector<uint8_t> BadHashSize = Valid;
  BadHashSize[offsetof(CS_CodeDirectory, hashSize)] = 31;
  Cases.emplace_back("hash encoding", BadHashSize);

  std::vector<uint8_t> BadSpecialProduct = Valid;
  writeBE32(BadSpecialProduct, offsetof(CS_CodeDirectory, nSpecialSlots),
            std::numeric_limits<uint32_t>::max());
  Cases.emplace_back("hash region", BadSpecialProduct);

  std::vector<uint8_t> BadCodeProduct = Valid;
  writeBE32(BadCodeProduct, offsetof(CS_CodeDirectory, nCodeSlots),
            std::numeric_limits<uint32_t>::max());
  Cases.emplace_back("hash region", BadCodeProduct);

  std::vector<uint8_t> BadCodeLimit = Valid;
  writeBE32(BadCodeLimit, offsetof(CS_CodeDirectory, codeLimit), UINT32_MAX);
  writeBE32(BadCodeLimit, offsetof(CS_CodeDirectory, codeLimit64), UINT32_MAX);
  Cases.emplace_back("codeLimit", BadCodeLimit);

  for (auto &[ExpectedDetail, Directory] : Cases) {
    SCOPED_TRACE(ExpectedDetail);
    llvm::Expected<Profile> Result = inspectBytes(
        thinMachO(superBlob({{CSSLOT_CODEDIRECTORY, std::move(Directory)}})));
    ASSERT_FALSE(Result);
    EXPECT_NE(takeError(Result).find(ExpectedDetail), std::string::npos);
  }
}

TEST(MachOSignaturePolicy, PrimarySpecialSlotHashesAreNotResignEligible) {
  using namespace llvm::MachO;
  std::vector<uint8_t> Directory = codeDirectory(CS_ADHOC, "com.example.tool");
  const uint32_t OldHashOffset = llvm::support::endian::read32be(
      Directory.data() + offsetof(CS_CodeDirectory, hashOffset));
  Directory.insert(Directory.begin() + OldHashOffset, CS_SHA256_LEN, 0);
  writeBE32(Directory, offsetof(CS_CodeDirectory, length),
            static_cast<uint32_t>(Directory.size()));
  writeBE32(Directory, offsetof(CS_CodeDirectory, hashOffset),
            OldHashOffset + CS_SHA256_LEN);
  writeBE32(Directory, offsetof(CS_CodeDirectory, nSpecialSlots), 1);

  llvm::Expected<Profile> Result = inspectBytes(
      thinMachO(superBlob({{CSSLOT_CODEDIRECTORY, std::move(Directory)}})));

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_FALSE(canTransactionallyAdHocResign(*Result));
}

TEST(MachOSignaturePolicy, CodeDirectoryUnclaimedBytesMustBeZero) {
  using namespace llvm::MachO;
  const std::vector<uint8_t> Valid =
      codeDirectory(CS_ADHOC, "com.example.tool");

  std::vector<uint8_t> HeaderGap = Valid;
  const uint32_t IdentifierOffset = llvm::support::endian::read32be(
      HeaderGap.data() + offsetof(CS_CodeDirectory, identOffset));
  const uint32_t HashOffset = llvm::support::endian::read32be(
      HeaderGap.data() + offsetof(CS_CodeDirectory, hashOffset));
  HeaderGap.insert(HeaderGap.begin() + IdentifierOffset, 4, 0);
  HeaderGap[IdentifierOffset + 1] = 0x5a;
  writeBE32(HeaderGap, offsetof(CS_CodeDirectory, length),
            static_cast<uint32_t>(HeaderGap.size()));
  writeBE32(HeaderGap, offsetof(CS_CodeDirectory, identOffset),
            IdentifierOffset + 4);
  writeBE32(HeaderGap, offsetof(CS_CodeDirectory, hashOffset), HashOffset + 4);

  std::vector<uint8_t> IdentifierGap = Valid;
  IdentifierGap.insert(IdentifierGap.begin() + HashOffset, 4, 0);
  IdentifierGap[HashOffset + 2] = 0x5a;
  writeBE32(IdentifierGap, offsetof(CS_CodeDirectory, length),
            static_cast<uint32_t>(IdentifierGap.size()));
  writeBE32(IdentifierGap, offsetof(CS_CodeDirectory, hashOffset),
            HashOffset + 4);

  for (std::vector<uint8_t> Directory : {HeaderGap, IdentifierGap}) {
    llvm::Expected<Profile> Result = inspectBytes(
        thinMachO(superBlob({{CSSLOT_CODEDIRECTORY, std::move(Directory)}})));
    ASSERT_FALSE(Result);
    EXPECT_NE(takeError(Result).find("nonzero unclaimed bytes"),
              std::string::npos);
  }
}

TEST(MachOSignaturePolicy, StrongCodeDirectorySemanticsFailClosed) {
  using namespace llvm::MachO;
  const std::vector<uint8_t> Valid =
      codeDirectory(CS_ADHOC, "com.example.tool");
  std::vector<std::pair<std::string, std::vector<uint8_t>>> Cases;

  for (size_t Offset : {offsetof(CS_CodeDirectory, platform),
                        offsetof(CS_CodeDirectory, spare2),
                        offsetof(CS_CodeDirectory, scatterOffset),
                        offsetof(CS_CodeDirectory, teamOffset),
                        offsetof(CS_CodeDirectory, spare3)}) {
    std::vector<uint8_t> Nonzero = Valid;
    if (Offset == offsetof(CS_CodeDirectory, platform))
      Nonzero[Offset] = 1;
    else
      writeBE32(Nonzero, Offset, 1);
    Cases.emplace_back("reserved policy field", std::move(Nonzero));
  }

  std::vector<uint8_t> WrongLimit = Valid;
  writeBE32(WrongLimit, offsetof(CS_CodeDirectory, codeLimit),
            sizeof(mach_header_64) + sizeof(linkedit_data_command) - 1);
  Cases.emplace_back("codeLimit", std::move(WrongLimit));

  std::vector<uint8_t> MissingCodeSlot = withCodeDirectoryHash(
      CS_ADHOC, "com.example.tool", CS_HASHTYPE_SHA256, CS_SHA256_LEN, 0);
  Cases.emplace_back("page coverage", std::move(MissingCodeSlot));

  std::vector<uint8_t> InfinitePage = Valid;
  InfinitePage[offsetof(CS_CodeDirectory, pageSize)] = 0;
  Cases.emplace_back("page size", std::move(InfinitePage));

  std::vector<uint8_t> OutOfBoundsExec = Valid;
  writeBE64(OutOfBoundsExec, offsetof(CS_CodeDirectory, execSegBase), 40);
  writeBE64(OutOfBoundsExec, offsetof(CS_CodeDirectory, execSegLimit), 16);
  Cases.emplace_back("executable segment", std::move(OutOfBoundsExec));

  for (auto &[ExpectedDetail, Directory] : Cases) {
    SCOPED_TRACE(ExpectedDetail);
    llvm::Expected<Profile> Result = inspectBytes(
        thinMachO(superBlob({{CSSLOT_CODEDIRECTORY, std::move(Directory)}})));
    ASSERT_FALSE(Result);
    EXPECT_NE(takeError(Result).find(ExpectedDetail), std::string::npos);
  }
}

TEST(MachOSignaturePolicy, WeakHashAndUnsafeExecFlagsAreNotEligible) {
  using namespace llvm::MachO;
  llvm::Expected<Profile> WeakHash = inspectBytes(thinMachO(
      superBlob({{CSSLOT_CODEDIRECTORY,
                  withCodeDirectoryHash(CS_ADHOC, "com.example.tool",
                                        CS_HASHTYPE_SHA1, CS_SHA1_LEN, 1)}})));
  ASSERT_TRUE(static_cast<bool>(WeakHash))
      << llvm::toString(WeakHash.takeError());
  EXPECT_FALSE(canTransactionallyAdHocResign(*WeakHash));

  std::vector<uint8_t> UnsafeExec = codeDirectory(CS_ADHOC, "com.example.tool");
  writeBE64(UnsafeExec, offsetof(CS_CodeDirectory, execSegFlags),
            CS_EXECSEG_JIT);
  llvm::Expected<Profile> Unsafe = inspectBytes(
      thinMachO(superBlob({{CSSLOT_CODEDIRECTORY, std::move(UnsafeExec)}})));
  ASSERT_TRUE(static_cast<bool>(Unsafe)) << llvm::toString(Unsafe.takeError());
  EXPECT_FALSE(canTransactionallyAdHocResign(*Unsafe));
}

TEST(MachOSignaturePolicy, EmbeddedSignatureMustTerminateItsSlice) {
  using namespace llvm::MachO;
  std::vector<uint8_t> Bytes = thinMachO(superBlob(
      {{CSSLOT_CODEDIRECTORY, codeDirectory(CS_ADHOC, "com.example.tool")}}));
  Bytes.push_back(0);

  llvm::Expected<Profile> Result = inspectBytes(Bytes);

  ASSERT_FALSE(Result);
  EXPECT_NE(takeError(Result).find("terminate the Mach-O slice"),
            std::string::npos);
}

TEST(MachOSignaturePolicy, MalformedSignatureStructuresAreRejected) {
  using namespace llvm::MachO;
  const std::vector<uint8_t> Valid = superBlob(
      {{CSSLOT_CODEDIRECTORY, codeDirectory(CS_ADHOC, "com.example.tool")}});
  std::vector<std::pair<std::string, std::vector<uint8_t>>> Cases;

  Cases.emplace_back("valid Mach-O container",
                     thinMachO(Valid, CPU_TYPE_X86_64, 2));
  Cases.emplace_back("valid Mach-O container",
                     thinMachOWithExtendedSignatureCommand());

  std::vector<uint8_t> BadSuperMagic = Valid;
  writeBE32(BadSuperMagic, offsetof(CS_SuperBlob, magic), 0x12345678);
  Cases.emplace_back("SuperBlob magic", thinMachO(BadSuperMagic));

  std::vector<uint8_t> BadSuperLength = Valid;
  writeBE32(BadSuperLength, offsetof(CS_SuperBlob, length),
            static_cast<uint32_t>(Valid.size() + 1));
  Cases.emplace_back("SuperBlob length", thinMachO(BadSuperLength));

  std::vector<uint8_t> BadPadding = Valid;
  BadPadding.push_back(0x5a);
  Cases.emplace_back("SuperBlob padding", thinMachO(BadPadding));

  std::vector<uint8_t> BadInternalTrailing = Valid;
  BadInternalTrailing.push_back(0x5a);
  writeBE32(BadInternalTrailing, offsetof(CS_SuperBlob, length),
            static_cast<uint32_t>(BadInternalTrailing.size()));
  Cases.emplace_back("nonzero unclaimed", thinMachO(BadInternalTrailing));

  std::vector<uint8_t> BadSlotCount = Valid;
  writeBE32(BadSlotCount, offsetof(CS_SuperBlob, count), UINT32_MAX);
  Cases.emplace_back("slot count", thinMachO(BadSlotCount));

  std::vector<uint8_t> OutOfRangeCommand = thinMachO(Valid);
  linkedit_data_command Command{};
  std::memcpy(&Command, OutOfRangeCommand.data() + sizeof(mach_header_64),
              sizeof(Command));
  Command.dataoff = static_cast<uint32_t>(OutOfRangeCommand.size());
  std::memcpy(OutOfRangeCommand.data() + sizeof(mach_header_64), &Command,
              sizeof(Command));
  Cases.emplace_back("valid Mach-O container", OutOfRangeCommand);

  std::vector<uint8_t> OverlappingSlots = superBlob(
      {{CSSLOT_CODEDIRECTORY, codeDirectory(CS_ADHOC, "com.example.tool")},
       {CSSLOT_REQUIREMENTS, genericBlob(CSMAGIC_REQUIREMENTS)}});
  const uint32_t FirstBlobOffset = llvm::support::endian::read32be(
      OverlappingSlots.data() + sizeof(CS_SuperBlob) +
      offsetof(CS_BlobIndex, offset));
  writeBE32(OverlappingSlots,
            sizeof(CS_SuperBlob) + sizeof(CS_BlobIndex) +
                offsetof(CS_BlobIndex, offset),
            FirstBlobOffset);
  Cases.emplace_back("slots overlap", thinMachO(OverlappingSlots));

  std::vector<uint8_t> MissingPrimary = superBlob(
      {{CSSLOT_ENTITLEMENTS, genericBlob(CSMAGIC_EMBEDDED_ENTITLEMENTS)}});
  Cases.emplace_back("primary CodeDirectory", thinMachO(MissingPrimary));

  std::vector<uint8_t> DuplicatePrimary = superBlob(
      {{CSSLOT_CODEDIRECTORY, codeDirectory(CS_ADHOC, "com.example.tool")},
       {CSSLOT_CODEDIRECTORY, codeDirectory(CS_ADHOC, "com.example.tool")}});
  Cases.emplace_back("duplicate signature slot", thinMachO(DuplicatePrimary));

  std::vector<uint8_t> UnboundedIdentifier = codeDirectory(CS_ADHOC, "x");
  const uint32_t IdentifierOffset = llvm::support::endian::read32be(
      UnboundedIdentifier.data() + offsetof(CS_CodeDirectory, identOffset));
  UnboundedIdentifier[IdentifierOffset + 1] = 'x';
  Cases.emplace_back(
      "identifier",
      thinMachO(superBlob({{CSSLOT_CODEDIRECTORY, UnboundedIdentifier}})));

  for (auto &[ExpectedDetail, Bytes] : Cases) {
    SCOPED_TRACE(ExpectedDetail);
    llvm::Expected<Profile> Result = inspectBytes(Bytes);
    ASSERT_FALSE(Result);
    const std::string Detail = takeError(Result);
    EXPECT_NE(Detail.find(ExpectedDetail), std::string::npos) << Detail;
  }
}

TEST(MachOSignaturePolicy, ConflictingCodeDirectoryIdentifiersAreRejected) {
  using namespace llvm::MachO;
  llvm::Expected<Profile> Result = inspectBytes(thinMachO(superBlob(
      {{CSSLOT_CODEDIRECTORY, codeDirectory(CS_ADHOC, "com.example.one")},
       {CSSLOT_ALTERNATE_CODEDIRECTORIES,
        codeDirectory(CS_ADHOC, "com.example.two")}})));

  ASSERT_FALSE(Result);
  EXPECT_NE(takeError(Result).find("identifiers disagree"), std::string::npos);
}

TEST(MachOSignaturePolicy, UniversalEligibilityRequiresEveryMatchingSlice) {
  using namespace llvm::MachO;
  const auto X64 =
      thinMachO(superBlob({{CSSLOT_CODEDIRECTORY,
                            codeDirectory(CS_ADHOC, "com.example.tool")}}),
                CPU_TYPE_X86_64);
  const auto ARM64 = thinMachO(
      superBlob(
          {{CSSLOT_CODEDIRECTORY,
            codeDirectory(CS_ADHOC | CS_LINKER_SIGNED, "com.example.tool")}}),
      CPU_TYPE_ARM64);
  llvm::Expected<Profile> Eligible = inspectBytes(fatMachO(X64, ARM64));

  ASSERT_TRUE(static_cast<bool>(Eligible))
      << llvm::toString(Eligible.takeError());
  EXPECT_TRUE(Eligible->Universal);
  ASSERT_EQ(Eligible->Slices.size(), 2u);
  EXPECT_TRUE(canTransactionallyAdHocResign(*Eligible));

  llvm::Expected<Profile> Mixed =
      inspectBytes(fatMachO(thinMachO({}, CPU_TYPE_X86_64), ARM64));
  ASSERT_TRUE(static_cast<bool>(Mixed)) << llvm::toString(Mixed.takeError());
  EXPECT_FALSE(canTransactionallyAdHocResign(*Mixed));

  const auto DifferentARM64 =
      thinMachO(superBlob({{CSSLOT_CODEDIRECTORY,
                            codeDirectory(CS_ADHOC, "com.example.other")}}),
                CPU_TYPE_ARM64);
  llvm::Expected<Profile> DifferentIdentity =
      inspectBytes(fatMachO(X64, DifferentARM64));
  ASSERT_TRUE(static_cast<bool>(DifferentIdentity))
      << llvm::toString(DifferentIdentity.takeError());
  EXPECT_FALSE(canTransactionallyAdHocResign(*DifferentIdentity));
}

TEST(MachOSignaturePolicy,
     CanonicalCodesignOutputIsSeparateFromInputResignEligibility) {
  using namespace llvm::MachO;
  llvm::Expected<Profile> Before = inspectBytes(thinMachO(superBlob(
      {{CSSLOT_CODEDIRECTORY,
        codeDirectory(CS_ADHOC | CS_LINKER_SIGNED, "com.example.tool")}})));
  llvm::Expected<Profile> After =
      inspectBytes(thinMachO(superBlob(canonicalCodesignOutputSlots())));

  ASSERT_TRUE(static_cast<bool>(Before)) << llvm::toString(Before.takeError());
  ASSERT_TRUE(static_cast<bool>(After)) << llvm::toString(After.takeError());
  ASSERT_EQ(After->Slices.size(), 1u);
  EXPECT_TRUE(After->Slices[0].HasCanonicalCodesignOutputShape);
  EXPECT_FALSE(canTransactionallyAdHocResign(*After));
  EXPECT_TRUE(isCanonicalCodesignAdHocOutput(*After));
  llvm::Error Validation =
      validateTransactionallyAdHocResigned(*Before, *After);
  EXPECT_FALSE(static_cast<bool>(Validation))
      << llvm::toString(std::move(Validation));
}

TEST(MachOSignaturePolicy,
     CodesignOutputSpecialHashesAndExactSlotSetAreAuthenticated) {
  using namespace llvm::MachO;
  const auto ExpectRejected = [](std::vector<Slot> Slots) {
    llvm::Expected<Profile> Result =
        inspectBytes(thinMachO(superBlob(std::move(Slots))));
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Slices.size(), 1u);
    EXPECT_FALSE(Result->Slices[0].HasCanonicalCodesignOutputShape);
    EXPECT_FALSE(isCanonicalCodesignAdHocOutput(*Result));
  };

  std::vector<Slot> BadRequirementsHash = canonicalCodesignOutputSlots();
  uint32_t HashOffset =
      llvm::support::endian::read32be(BadRequirementsHash[0].Blob.data() +
                                      offsetof(CS_CodeDirectory, hashOffset));
  BadRequirementsHash[0].Blob[HashOffset - 2 * CS_SHA256_LEN] ^= 0x5a;
  ExpectRejected(std::move(BadRequirementsHash));

  std::vector<Slot> NonzeroSlotOne = canonicalCodesignOutputSlots();
  HashOffset = llvm::support::endian::read32be(
      NonzeroSlotOne[0].Blob.data() + offsetof(CS_CodeDirectory, hashOffset));
  NonzeroSlotOne[0].Blob[HashOffset - CS_SHA256_LEN] = 1;
  ExpectRejected(std::move(NonzeroSlotOne));

  std::vector<Slot> MissingWrapper = canonicalCodesignOutputSlots();
  MissingWrapper.pop_back();
  ExpectRejected(std::move(MissingWrapper));

  std::vector<Slot> ExtraSlot = canonicalCodesignOutputSlots();
  ExtraSlot.push_back({CSSLOT_APPLICATION, genericBlob(0xfade0c03)});
  ExpectRejected(std::move(ExtraSlot));
}

TEST(MachOSignaturePolicy,
     CodesignOutputRequiresExactEmptyRequirementsAndCMSWrapper) {
  using namespace llvm::MachO;
  const auto ExpectRejected = [](std::vector<Slot> Slots) {
    llvm::Expected<Profile> Result =
        inspectBytes(thinMachO(superBlob(std::move(Slots))));
    if (!Result)
      return llvm::consumeError(Result.takeError());
    ASSERT_EQ(Result->Slices.size(), 1u);
    EXPECT_FALSE(Result->Slices[0].HasCanonicalCodesignOutputShape);
    EXPECT_FALSE(isCanonicalCodesignAdHocOutput(*Result));
  };

  std::vector<Slot> WrongRequirementsMagic = canonicalCodesignOutputSlots();
  writeBE32(WrongRequirementsMagic[1].Blob, offsetof(CS_SuperBlob, magic),
            CSMAGIC_REQUIREMENT);
  ExpectRejected(std::move(WrongRequirementsMagic));

  std::vector<Slot> NonemptyRequirements = canonicalCodesignOutputSlots();
  writeBE32(NonemptyRequirements[1].Blob, offsetof(CS_SuperBlob, count), 1);
  ExpectRejected(std::move(NonemptyRequirements));

  std::vector<Slot> LongRequirements = canonicalCodesignOutputSlots();
  LongRequirements[1].Blob.resize(sizeof(CS_SuperBlob) + 4, 0);
  writeBE32(LongRequirements[1].Blob, offsetof(CS_SuperBlob, length),
            static_cast<uint32_t>(LongRequirements[1].Blob.size()));
  ExpectRejected(std::move(LongRequirements));

  std::vector<Slot> WrongWrapperMagic = canonicalCodesignOutputSlots();
  writeBE32(WrongWrapperMagic[2].Blob, offsetof(CS_CodeDirectory, magic),
            CSMAGIC_EMBEDDED_SIGNATURE);
  ExpectRejected(std::move(WrongWrapperMagic));

  std::vector<Slot> NonemptyWrapper = canonicalCodesignOutputSlots();
  NonemptyWrapper[2].Blob.resize(12, 0);
  writeBE32(NonemptyWrapper[2].Blob, offsetof(CS_CodeDirectory, length), 12);
  ExpectRejected(std::move(NonemptyWrapper));
}

TEST(MachOSignaturePolicy, CanonicalCodesignOutputForbidsSuperBlobGaps) {
  using namespace llvm::MachO;
  std::vector<uint8_t> Signature = superBlob(canonicalCodesignOutputSlots());
  const uint32_t Count = llvm::support::endian::read32be(
      Signature.data() + offsetof(CS_SuperBlob, count));
  const size_t IndexEnd = sizeof(CS_SuperBlob) + Count * sizeof(CS_BlobIndex);
  Signature.insert(Signature.begin() + IndexEnd, 1, 0);
  writeBE32(Signature, offsetof(CS_SuperBlob, length),
            static_cast<uint32_t>(Signature.size()));
  for (uint32_t Index = 0; Index < Count; ++Index) {
    const size_t OffsetField = sizeof(CS_SuperBlob) +
                               Index * sizeof(CS_BlobIndex) +
                               offsetof(CS_BlobIndex, offset);
    writeBE32(Signature, OffsetField,
              llvm::support::endian::read32be(Signature.data() + OffsetField) +
                  1);
  }

  llvm::Expected<Profile> Result = inspectBytes(thinMachO(Signature));

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_FALSE(Result->Slices[0].HasCanonicalCodesignOutputShape);
  EXPECT_FALSE(isCanonicalCodesignAdHocOutput(*Result));
}

TEST(MachOSignaturePolicy, ResignedArtifactMustPreserveEveryIdentifier) {
  using namespace llvm::MachO;
  llvm::Expected<Profile> Before = inspectBytes(thinMachO(superBlob(
      {{CSSLOT_CODEDIRECTORY,
        codeDirectory(CS_ADHOC | CS_LINKER_SIGNED, "com.example.tool")}})));
  llvm::Expected<Profile> After =
      inspectBytes(thinMachO(superBlob(canonicalCodesignOutputSlots())));
  ASSERT_TRUE(static_cast<bool>(Before)) << llvm::toString(Before.takeError());
  ASSERT_TRUE(static_cast<bool>(After)) << llvm::toString(After.takeError());
  llvm::Error Valid = validateTransactionallyAdHocResigned(*Before, *After);
  EXPECT_FALSE(static_cast<bool>(Valid));

  llvm::Expected<Profile> WrongIdentity = inspectBytes(
      thinMachO(superBlob(canonicalCodesignOutputSlots("com.example.other"))));
  ASSERT_TRUE(static_cast<bool>(WrongIdentity))
      << llvm::toString(WrongIdentity.takeError());
  llvm::Error Error =
      validateTransactionallyAdHocResigned(*Before, *WrongIdentity);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("identifier changed"),
            std::string::npos);

  llvm::Expected<Profile> StrongOutput =
      inspectBytes(thinMachO(superBlob(canonicalCodesignOutputSlots(
          "com.example.tool", CS_ADHOC | CS_RUNTIME))));
  ASSERT_TRUE(static_cast<bool>(StrongOutput))
      << llvm::toString(StrongOutput.takeError());
  llvm::Error StrongError =
      validateTransactionallyAdHocResigned(*Before, *StrongOutput);
  ASSERT_TRUE(static_cast<bool>(StrongError));
  EXPECT_NE(llvm::toString(std::move(StrongError))
                .find("not a canonical codesign ad-hoc"),
            std::string::npos);

  std::vector<Slot> ChangedExecSlots = canonicalCodesignOutputSlots();
  std::vector<uint8_t> &ChangedExecDirectory = ChangedExecSlots[0].Blob;
  writeBE64(ChangedExecDirectory, offsetof(CS_CodeDirectory, execSegFlags), 0);
  llvm::Expected<Profile> ChangedExec =
      inspectBytes(thinMachO(superBlob(std::move(ChangedExecSlots))));
  ASSERT_TRUE(static_cast<bool>(ChangedExec))
      << llvm::toString(ChangedExec.takeError());
  llvm::Error ExecError =
      validateTransactionallyAdHocResigned(*Before, *ChangedExec);
  ASSERT_TRUE(static_cast<bool>(ExecError));
  EXPECT_NE(llvm::toString(std::move(ExecError))
                .find("executable-segment policy changed"),
            std::string::npos);
}

} // namespace
