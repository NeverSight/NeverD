//===- SBFMalformedCorpusTests.cpp - Hostile SBF input corpus ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFFixtureBuilder.h"
#include "gtest/gtest.h"

#include "neverd/support/BinaryLoading.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

using ELFT = llvm::object::ELF64LE;
using Elf_Ehdr = ELFT::Ehdr;
using Elf_Phdr = ELFT::Phdr;
using Elf_Shdr = ELFT::Shdr;
using EncodedInstruction = std::array<uint8_t, kInstructionSize>;

class TemporaryELF {
public:
  explicit TemporaryELF(llvm::ArrayRef<uint8_t> Bytes) {
    Error =
        llvm::sys::fs::createTemporaryFile("neverd-sbf-malformed", "so", Path);
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

template <typename T>
T readRecord(llvm::ArrayRef<uint8_t> Bytes, size_t Offset) {
  EXPECT_LE(sizeof(T), Bytes.size() - std::min(Offset, Bytes.size()));
  T Record{};
  if (Offset <= Bytes.size() && sizeof(T) <= Bytes.size() - Offset)
    std::memcpy(&Record, Bytes.data() + Offset, sizeof(T));
  return Record;
}

template <typename T>
void writeRecord(std::vector<uint8_t> &Bytes, size_t Offset, const T &Record) {
  ASSERT_LE(Offset, Bytes.size());
  ASSERT_LE(sizeof(T), Bytes.size() - Offset);
  std::memcpy(Bytes.data() + Offset, &Record, sizeof(T));
}

EncodedInstruction encode(Opcode ID, uint8_t Dst = 0, uint8_t Src = 0,
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

std::vector<uint8_t>
instructionBytes(std::initializer_list<EncodedInstruction> Instructions) {
  std::vector<uint8_t> Bytes;
  for (const EncodedInstruction &Instruction : Instructions)
    Bytes.insert(Bytes.end(), Instruction.begin(), Instruction.end());
  return Bytes;
}

llvm::Expected<SBFProgram> loadAndAnalyze(llvm::ArrayRef<uint8_t> Bytes,
                                          const AnalyzeOptions &Options = {}) {
  TemporaryELF File(Bytes);
  if (File.error())
    return llvm::errorCodeToError(File.error());
  auto Image = loadBinary(File.path().str());
  if (!Image)
    return Image.takeError();
  return analyze(*Image, Options);
}

void expectLoadRejected(llvm::ArrayRef<uint8_t> Bytes) {
  TemporaryELF File(Bytes);
  ASSERT_FALSE(File.error()) << File.error().message();
  auto Image = loadBinary(File.path().str());
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_FALSE(Error.empty());
}

void expectAnalyzeRejected(std::initializer_list<EncodedInstruction> Text) {
  test::StrictELFOptions Options;
  Options.Text = instructionBytes(Text);
  auto Program = loadAndAnalyze(test::buildStrictELF(Options));
  ASSERT_FALSE(static_cast<bool>(Program));
  const std::string Error = llvm::toString(Program.takeError());
  EXPECT_FALSE(Error.empty());
  EXPECT_NE(Error.find("sbf"), std::string::npos) << Error;
}

TEST(SBFMalformedCorpus, RejectsHostileELFTableAndSegmentRanges) {
  using Mutation = std::function<void(std::vector<uint8_t> &)>;
  const std::vector<std::pair<std::string, Mutation>> Mutations{
      {"program-header-table-overflow",
       [](std::vector<uint8_t> &Bytes) {
         Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
         Header.e_phoff = std::numeric_limits<uint64_t>::max();
         writeRecord(Bytes, 0, Header);
       }},
      {"wrong-program-header-size",
       [](std::vector<uint8_t> &Bytes) {
         Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
         Header.e_phentsize = sizeof(Elf_Phdr) - 1;
         writeRecord(Bytes, 0, Header);
       }},
      {"segment-file-range-overflow",
       [](std::vector<uint8_t> &Bytes) {
         const Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
         Elf_Phdr Text = readRecord<Elf_Phdr>(Bytes, Header.e_phoff);
         Text.p_offset = std::numeric_limits<uint64_t>::max();
         writeRecord(Bytes, Header.e_phoff, Text);
       }},
      {"segment-memory-smaller-than-file",
       [](std::vector<uint8_t> &Bytes) {
         const Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
         Elf_Phdr Text = readRecord<Elf_Phdr>(Bytes, Header.e_phoff);
         Text.p_memsz = Text.p_filesz - 1;
         writeRecord(Bytes, Header.e_phoff, Text);
       }},
      {"misaligned-text-address",
       [](std::vector<uint8_t> &Bytes) {
         const Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
         Elf_Phdr Text = readRecord<Elf_Phdr>(Bytes, Header.e_phoff);
         Text.p_vaddr = static_cast<uint64_t>(Text.p_vaddr) + 1;
         Text.p_paddr = static_cast<uint64_t>(Text.p_paddr) + 1;
         writeRecord(Bytes, Header.e_phoff, Text);
       }},
      {"entry-outside-text",
       [](std::vector<uint8_t> &Bytes) {
         Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
         Header.e_entry += kMemoryRegionSize;
         writeRecord(Bytes, 0, Header);
       }},
      {"unsupported-version",
       [](std::vector<uint8_t> &Bytes) {
         Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
         Header.e_flags = static_cast<uint32_t>(Version::Reserved);
         writeRecord(Bytes, 0, Header);
       }},
      {"unsupported-machine",
       [](std::vector<uint8_t> &Bytes) {
         Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
         Header.e_machine = std::numeric_limits<uint16_t>::max();
         writeRecord(Bytes, 0, Header);
       }},
  };

  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    std::vector<uint8_t> Bytes = test::buildStrictELF();
    Mutate(Bytes);
    expectLoadRejected(Bytes);
  }
}

TEST(SBFMalformedCorpus, RejectsOverlappingStrictRuntimeSegments) {
  std::vector<uint8_t> Bytes = test::buildStrictELF({.AddRodata = true});
  const Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
  ASSERT_EQ(Header.e_phnum, 2u);
  Elf_Phdr Rodata = readRecord<Elf_Phdr>(Bytes, Header.e_phoff);
  Rodata.p_vaddr = kBytecodeStart;
  Rodata.p_paddr = kBytecodeStart;
  writeRecord(Bytes, Header.e_phoff, Rodata);
  expectLoadRejected(Bytes);
}

TEST(SBFMalformedCorpus, RejectsOutOfRangeLegacySectionNameTable) {
  std::vector<uint8_t> Bytes = test::buildLegacyELF();
  const Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
  ASSERT_LT(Header.e_shstrndx, Header.e_shnum);
  const size_t StringTableOffset =
      Header.e_shoff + Header.e_shstrndx * sizeof(Elf_Shdr);
  Elf_Shdr Strings = readRecord<Elf_Shdr>(Bytes, StringTableOffset);
  Strings.sh_offset = std::numeric_limits<uint64_t>::max();
  writeRecord(Bytes, StringTableOffset, Strings);
  expectLoadRejected(Bytes);
}

TEST(SBFMalformedCorpus, KeepsMalformedOptionalMetadataOutOfRuntimeLoading) {
  std::vector<uint8_t> Bytes = test::buildStrictELF();
  Elf_Ehdr Header = readRecord<Elf_Ehdr>(Bytes, 0);
  Header.e_shoff = std::numeric_limits<uint64_t>::max();
  Header.e_shnum = 1;
  Header.e_shstrndx = llvm::ELF::SHN_UNDEF;
  writeRecord(Bytes, 0, Header);

  TemporaryELF File(Bytes);
  ASSERT_FALSE(File.error()) << File.error().message();
  auto Image = loadBinary(File.path().str());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->SBF.has_value());
  EXPECT_EQ(Image->SBF->DebugEnrichment, DebugEnrichmentStatus::Malformed);
  auto Program = analyze(*Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Result = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Returned);
}

TEST(SBFMalformedCorpus, RejectsInvalidInstructionEncodingsAndRegisters) {
  EncodedInstruction Unknown{};
  Unknown[kOpcodeOffset] = 0xff;
  expectAnalyzeRejected({Unknown});
  expectAnalyzeRejected(
      {encode(Opcode::MOV64_REG, kRegisterCount, 0), encode(Opcode::EXIT)});
  expectAnalyzeRejected(
      {encode(Opcode::MOV64_REG, 0, kRegisterCount), encode(Opcode::EXIT)});
  expectAnalyzeRejected(
      {encode(Opcode::ADD64_IMM, kFramePointerRegister, 0, 0, 8),
       encode(Opcode::EXIT)});
}

TEST(SBFMalformedCorpus, RejectsMalformedLDDWAndControlFlow) {
  expectAnalyzeRejected({encode(Opcode::LDDW, 0)});

  EncodedInstruction BadContinuation = encode(Opcode::EXIT);
  expectAnalyzeRejected(
      {encode(Opcode::LDDW, 0), BadContinuation, encode(Opcode::EXIT)});

  EncodedInstruction Continuation{};
  expectAnalyzeRejected({encode(Opcode::LDDW, 0), Continuation,
                         encode(Opcode::JA, 0, 0, -2), encode(Opcode::EXIT)});
  expectAnalyzeRejected(
      {encode(Opcode::JA, 0, 0, std::numeric_limits<int16_t>::max())});
}

TEST(SBFMalformedCorpus, RejectsInvalidImmediateDomains) {
  expectAnalyzeRejected(
      {encode(Opcode::DIV64_IMM, 0, 0, 0, 0), encode(Opcode::EXIT)});
  expectAnalyzeRejected(
      {encode(Opcode::LSH64_IMM, 0, 0, 0, 64), encode(Opcode::EXIT)});
  expectAnalyzeRejected(
      {encode(Opcode::BE, 0, 0, 0, 24), encode(Opcode::EXIT)});
}

} // namespace
} // namespace neverd::sbf
