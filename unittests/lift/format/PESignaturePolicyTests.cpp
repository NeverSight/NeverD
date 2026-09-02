//===- PESignaturePolicyTests.cpp - PE signing policy tests -------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/PESignaturePolicy.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace neverd::pe_signature;

constexpr size_t DOSOffset = 0;
constexpr size_t PEOffset = 0x80;
constexpr size_t TextFileOffset = 0x200;
constexpr size_t CertificateOffset = 0x400;
constexpr uint32_t TextRVA = 0x1000;
constexpr size_t DirectoryCount = 16;
constexpr size_t FileHeaderOffset = PEOffset + 4;
constexpr size_t OptionalHeaderOffset =
    FileHeaderOffset + sizeof(coff_file_header);
constexpr size_t DirectoriesOffset =
    OptionalHeaderOffset + sizeof(pe32plus_header);
constexpr size_t SectionHeadersOffset =
    DirectoriesOffset + DirectoryCount * sizeof(data_directory);

template <typename T>
void writeObject(std::vector<uint8_t> &Bytes, size_t Offset, const T &Object) {
  ASSERT_LE(Offset + sizeof(T), Bytes.size());
  std::memcpy(Bytes.data() + Offset, &Object, sizeof(T));
}

void writeLE16(std::vector<uint8_t> &Bytes, size_t Offset, uint16_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write16le(Bytes.data() + Offset, Value);
}

void writeLE32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write32le(Bytes.data() + Offset, Value);
}

std::vector<uint8_t> makePE() {
  std::vector<uint8_t> Bytes(CertificateOffset, 0);
  dos_header DOS{};
  DOS.Magic[0] = 'M';
  DOS.Magic[1] = 'Z';
  DOS.AddressOfNewExeHeader = PEOffset;
  writeObject(Bytes, DOSOffset, DOS);
  Bytes[PEOffset] = 'P';
  Bytes[PEOffset + 1] = 'E';

  coff_file_header File{};
  File.Machine = IMAGE_FILE_MACHINE_AMD64;
  File.NumberOfSections = 1;
  File.SizeOfOptionalHeader =
      sizeof(pe32plus_header) + DirectoryCount * sizeof(data_directory);
  File.Characteristics =
      IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
  writeObject(Bytes, FileHeaderOffset, File);

  pe32plus_header Optional{};
  Optional.Magic = PE32Header::PE32_PLUS;
  Optional.SizeOfCode = 0x200;
  Optional.AddressOfEntryPoint = TextRVA;
  Optional.BaseOfCode = TextRVA;
  Optional.ImageBase = 0x140000000ULL;
  Optional.SectionAlignment = 0x1000;
  Optional.FileAlignment = 0x200;
  Optional.MajorOperatingSystemVersion = 6;
  Optional.MajorSubsystemVersion = 6;
  Optional.SizeOfImage = 0x2000;
  Optional.SizeOfHeaders = 0x200;
  Optional.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
  Optional.SizeOfStackReserve = 0x100000;
  Optional.SizeOfStackCommit = 0x1000;
  Optional.SizeOfHeapReserve = 0x100000;
  Optional.SizeOfHeapCommit = 0x1000;
  Optional.NumberOfRvaAndSize = DirectoryCount;
  writeObject(Bytes, OptionalHeaderOffset, Optional);

  std::array<data_directory, DirectoryCount> Directories{};
  for (size_t Index = 0; Index < Directories.size(); ++Index)
    writeObject(Bytes, DirectoriesOffset + Index * sizeof(data_directory),
                Directories[Index]);

  coff_section Text{};
  std::memcpy(Text.Name, ".text", 5);
  Text.VirtualSize = 1;
  Text.VirtualAddress = TextRVA;
  Text.SizeOfRawData = 0x200;
  Text.PointerToRawData = TextFileOffset;
  Text.Characteristics =
      IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
  writeObject(Bytes, SectionHeadersOffset, Text);
  Bytes[TextFileOffset] = 0xc3;
  return Bytes;
}

void setCertificateDirectory(std::vector<uint8_t> &Bytes, uint32_t Offset,
                             uint32_t Size) {
  data_directory Directory{};
  Directory.RelativeVirtualAddress = Offset;
  Directory.Size = Size;
  writeObject(Bytes,
              DirectoriesOffset + CERTIFICATE_TABLE * sizeof(data_directory),
              Directory);
}

void appendCertificate(std::vector<uint8_t> &Bytes, uint32_t Length,
                       uint16_t Revision = 0x0200,
                       uint16_t CertificateType = 0x0002) {
  const size_t Start = Bytes.size();
  const size_t AlignedLength = (uint64_t(Length) + 7) & ~uint64_t(7);
  Bytes.resize(Start + AlignedLength, 0);
  writeLE32(Bytes, Start, Length);
  writeLE16(Bytes, Start + 4, Revision);
  writeLE16(Bytes, Start + 6, CertificateType);
  for (size_t Index = Start + 8; Index < Start + Length; ++Index)
    Bytes[Index] = 0xa5;
}

std::string takeError(llvm::Expected<Profile> &Result) {
  return llvm::toString(Result.takeError());
}

TEST(PESignaturePolicy, UnsignedImageHasNoCertificateTable) {
  llvm::Expected<Profile> Result = inspect(makePE());

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->SignatureKind, Kind::Unsigned);
  EXPECT_EQ(Result->CertificateCount, 0u);
}

TEST(PESignaturePolicy, ValidCertificateRecordsAreDetected) {
  std::vector<uint8_t> Bytes = makePE();
  appendCertificate(Bytes, 12);
  appendCertificate(Bytes, 8, 0x0100, 0x0001);
  setCertificateDirectory(
      Bytes, CertificateOffset,
      static_cast<uint32_t>(Bytes.size() - CertificateOffset));

  llvm::Expected<Profile> Result = inspect(Bytes);

  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->SignatureKind, Kind::Authenticode);
  EXPECT_EQ(Result->CertificateTableOffset, CertificateOffset);
  EXPECT_EQ(Result->CertificateTableSize, 24u);
  EXPECT_EQ(Result->CertificateCount, 2u);
}

TEST(PESignaturePolicy, AmbiguousSecurityDirectoryFailsClosed) {
  std::vector<std::pair<std::string, std::vector<uint8_t>>> Cases;

  std::vector<uint8_t> MissingSize = makePE();
  setCertificateDirectory(MissingSize, CertificateOffset, 0);
  Cases.emplace_back("both be zero", std::move(MissingSize));

  std::vector<uint8_t> MissingOffset = makePE();
  setCertificateDirectory(MissingOffset, 0, 8);
  Cases.emplace_back("both be zero", std::move(MissingOffset));

  std::vector<uint8_t> Unaligned = makePE();
  Unaligned.resize(CertificateOffset + 16, 0);
  setCertificateDirectory(Unaligned, CertificateOffset + 1, 8);
  Cases.emplace_back("8-byte aligned", std::move(Unaligned));

  std::vector<uint8_t> OutOfBounds = makePE();
  setCertificateDirectory(OutOfBounds, CertificateOffset, 8);
  Cases.emplace_back("outside the file", std::move(OutOfBounds));

  for (auto &[Expected, Bytes] : Cases) {
    SCOPED_TRACE(Expected);
    llvm::Expected<Profile> Result = inspect(Bytes);
    ASSERT_FALSE(Result);
    const std::string Detail = takeError(Result);
    EXPECT_NE(Detail.find(Expected), std::string::npos) << Detail;
  }
}

TEST(PESignaturePolicy, CertificateRecordsMustExactlyConsumeTheTable) {
  std::vector<std::pair<std::string, std::vector<uint8_t>>> Cases;

  std::vector<uint8_t> ShortHeader = makePE();
  ShortHeader.resize(CertificateOffset + 4, 0);
  setCertificateDirectory(ShortHeader, CertificateOffset, 4);
  Cases.emplace_back("header", std::move(ShortHeader));

  std::vector<uint8_t> ShortLength = makePE();
  ShortLength.resize(CertificateOffset + 8, 0);
  writeLE32(ShortLength, CertificateOffset, 7);
  setCertificateDirectory(ShortLength, CertificateOffset, 8);
  Cases.emplace_back("length", std::move(ShortLength));

  std::vector<uint8_t> TruncatedRecord = makePE();
  TruncatedRecord.resize(CertificateOffset + 8, 0);
  writeLE32(TruncatedRecord, CertificateOffset, 16);
  setCertificateDirectory(TruncatedRecord, CertificateOffset, 8);
  Cases.emplace_back("length", std::move(TruncatedRecord));

  std::vector<uint8_t> MissingPadding = makePE();
  MissingPadding.resize(CertificateOffset + 12, 0);
  writeLE32(MissingPadding, CertificateOffset, 12);
  setCertificateDirectory(MissingPadding, CertificateOffset, 12);
  Cases.emplace_back("aligned record", std::move(MissingPadding));

  std::vector<uint8_t> NonzeroPadding = makePE();
  appendCertificate(NonzeroPadding, 12);
  NonzeroPadding.back() = 0x5a;
  setCertificateDirectory(NonzeroPadding, CertificateOffset, 16);
  Cases.emplace_back("padding", std::move(NonzeroPadding));

  for (auto &[Expected, Bytes] : Cases) {
    SCOPED_TRACE(Expected);
    llvm::Expected<Profile> Result = inspect(Bytes);
    ASSERT_FALSE(Result);
    const std::string Detail = takeError(Result);
    EXPECT_NE(Detail.find(Expected), std::string::npos) << Detail;
  }
}

} // namespace
