//===- PPC64ELFv2Tests.cpp - Global and local entry contracts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify the entry metadata, reaching registers and call identity used by
/// the two byte orders of PPC64 ELFv2.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/loader/ELF/ELFLoader.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ObjectYAML/yaml2obj.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

using namespace neverd;
namespace {
#include "PPC64ELFv2Fixture.inc"

struct Fixture {
  bool Little;
  unsigned Other = SeparateEntries, Size = 16;
  std::vector<uint32_t> Words = {std::begin(Code), std::end(Code)};
  std::string Aliases;
  llvm::Expected<BinaryImage> load() {
    llvm::SmallVector<uint8_t> Bytes;
    auto Order = Little ? llvm::endianness::little : llvm::endianness::big;
    for (uint32_t Word : Words) {
      uint8_t Encoded[4];
      llvm::support::endian::write32(Encoded, Word, Order);
      Bytes.append(std::begin(Encoded), std::end(Encoded));
    }
    const auto YAML = llvm::formatv(R"(--- !ELF
FileHeader: {{ Class: ELFCLASS64, Data: {0}, Type: ET_EXEC, Machine: EM_PPC64, Entry: 0x10000 }
ProgramHeaders:
  - {{ Type: PT_LOAD, Flags: [ PF_R, PF_X ], VAddr: 0x10000, FirstSec: .text, LastSec: .text }
  - {{ Type: PT_LOAD, Flags: [ PF_R, PF_W ], VAddr: 0x30000, FirstSec: .data, LastSec: .data }
Sections:
  - {{ Name: .text, Type: SHT_PROGBITS, Flags: [ SHF_ALLOC, SHF_EXECINSTR ], Address: 0x10000, Content: "{1}" }
  - {{ Name: .data, Type: SHT_PROGBITS, Flags: [ SHF_ALLOC, SHF_WRITE ], Address: 0x30000, Content: "0102030405060708" }
Symbols:
  - {{ Name: caller, Type: STT_FUNC, Binding: STB_GLOBAL, Section: .text, Value: 0x10000, Size: 56, Other: [ 96 ] }
  - {{ Name: callee, Type: STT_FUNC, Binding: STB_GLOBAL, Section: .text, Value: 0x10040, Size: {2}, Other: [ {3} ] }
{4})",
                                    Little ? "ELFDATA2LSB" : "ELFDATA2MSB",
                                    llvm::toHex(Bytes), Size, Other, Aliases)
                          .str();
    llvm::SmallVector<char, 0> Storage;
    std::string Diagnostic;
    auto Obj = llvm::yaml::yaml2ObjectFile(
        Storage, YAML, [&](const llvm::Twine &T) { Diagnostic = T.str(); });
    if (!Obj)
      return llvm::createStringError("fixture: %s", Diagnostic.c_str());
    llvm::support::endian::write32(
        Storage.data() + offsetof(llvm::object::ELF64LE::Ehdr, e_flags), 2,
        Order);
    return ELFLoader().loadBuffer(Obj->getMemoryBufferRef());
  }
};
MedFunc convert(const BinaryImage &I, va_t Entry = Text) {
  Decoder D;
  EXPECT_TRUE(D.init(I));
  auto Low = CFGBuilder().build(I, D, Entry, "caller");
  LowToMedConverter Converter;
  Converter.setBinaryImage(&I);
  return Converter.convert(Low, I.Arch, BinaryFormat::ELF);
}

TEST(PPC64ELFv2, LocalCallUsesGlobalIdentityAndTheCallersReachingTOC) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    auto Image = F.load();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->PPC64EntryContexts.at(Text).TOC, Data);
    EXPECT_EQ(Image->ppc64GlobalEntry(Callee + 8), Callee);
    EXPECT_FALSE(Image->isFunctionEntryCandidateAt(Callee + 8));
    EXPECT_TRUE(Image->isFunctionEntryCandidateAt(Callee));
    auto Fn = convert(*Image);
    bool Resolved = false, ReadsData = false;
    for (const auto &B : Fn.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.NumInputs && Op.Inputs[0].isConst()) {
          Resolved |=
              Op.Opcode == NdOp::CALL && Op.Inputs[0].ConstVal == Callee;
          ReadsData |= Op.Opcode == NdOp::LOAD && Op.Inputs[0].ConstVal == Data;
        }
    EXPECT_TRUE(Resolved);
    EXPECT_TRUE(ReadsData);
    F.Words[5] = ChangeTOC;
    Image = F.load();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_THROW(convert(*Image), LowToMedConversionError);
  }
}

TEST(PPC64ELFv2, GlobalCallNeedsR12AndLocalCallNeedsKnownSetup) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    F.Words[6] = BranchToGlobal;
    auto Image = F.load();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_THROW(convert(*Image), LowToMedConversionError);
    F.Words[6] = Code[6];
    F.Words[16] = ReadUnknownRegister;
    Image = F.load();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_FALSE(Image->PPC64EntryContexts.at(Callee).TOC);
    EXPECT_THROW(convert(*Image), LowToMedConversionError);
  }
}

TEST(PPC64ELFv2, SingleEntryDistinguishesPreservedAndClobberedR2) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    F.Words[6] = BranchToGlobal;
    F.Words[16] = Return;
    for (unsigned Other : {0u, ClobberingSingleEntry}) {
      F.Other = Other;
      auto Image = F.load();
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      const auto &C = Image->PPC64EntryContexts.at(Callee);
      EXPECT_EQ(C.LocalEntryOffset, 0u);
      EXPECT_EQ(C.PreservesTOC, Other == 0);
      if (Other)
        EXPECT_THROW(convert(*Image), LowToMedConversionError);
      else
        EXPECT_NO_THROW(convert(*Image));
    }
  }
}

TEST(PPC64ELFv2, PrefixedStubEstablishesTheCalleesGlobalEntry) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    const auto First = F.Words.size();
    F.Words[6] = CallStub;
    F.Words.insert(F.Words.end(), std::begin(GlobalStub), std::end(GlobalStub));
    auto Image = F.load();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    va_t Stub = Text + First * sizeof(uint32_t);
    auto It = Image->PPC64LinkageStubs.find(Stub);
    ASSERT_NE(It, Image->PPC64LinkageStubs.end());
    EXPECT_EQ(It->second.Target, Callee);
    EXPECT_EQ(It->second.IncomingTOC, InvalidVA);
    F.Words[First] = NonRelativePrefix;
    Image = F.load();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_FALSE(Image->PPC64LinkageStubs.contains(Stub));
  }
}

TEST(PPC64ELFv2, LargeModelEntryReadsASignedTargetEndianLiteral) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    for (va_t TOC : {Data, Text}) {
      const uint64_t Difference = TOC - Callee;
      F.Words[14] = Little ? uint32_t(Difference) : uint32_t(Difference >> 32);
      F.Words[15] = Little ? uint32_t(Difference >> 32) : uint32_t(Difference);
      F.Words[16] = LoadTOCLiteral;
      F.Words[17] = AddEntryToTOC;
      auto Image = F.load();
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      EXPECT_EQ(Image->PPC64EntryContexts.at(Callee).TOC, TOC);
    }
  }
}

TEST(PPC64ELFv2, RejectsReservedOutOfBoundsAndConflictingEntries) {
  for (bool Little : {false, true}) {
    for (unsigned Variant = 0; Variant < 4; ++Variant) {
      Fixture F{Little};
      if (Variant == 0)
        F.Other = ReservedEntry;
      if (Variant == 1)
        F.Size = 9;
      if (Variant >= 2)
        F.Aliases =
            llvm::formatv("  - {{ Name: alias, Type: STT_FUNC, Section: .text, "
                          "Value: {0}, Other: [ 0 ] }\n",
                          Variant == 2 ? Callee : Callee + 8)
                .str();
      auto Image = F.load();
      ASSERT_FALSE(bool(Image));
      EXPECT_TRUE(llvm::StringRef(llvm::toString(Image.takeError()))
                      .contains("PPC64 ELFv2"));
    }
  }
}
} // namespace
