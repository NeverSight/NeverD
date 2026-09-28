//===- PatternGeneratorTests.cpp - .pat generation tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternGenerator.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/ObjectFile.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::sigs;
using namespace llvm;

namespace {

/// A single-section COFF object assembled in memory.
///
/// Symbol 0 is the section symbol (with its auxiliary record, so it takes
/// two table slots); symbols added later follow it. The one undefined
/// symbol every relocation points at is added by build().
class COFFObjectBuilder {
public:
  struct Symbol {
    std::string Name;
    uint32_t Value;
    int16_t Section;
    uint16_t Type;
    uint8_t Class;
  };

  COFFObjectBuilder(uint16_t Machine, std::vector<uint8_t> Code,
                    uint32_t Characteristics = COFF::IMAGE_SCN_CNT_CODE |
                                               COFF::IMAGE_SCN_MEM_EXECUTE |
                                               COFF::IMAGE_SCN_MEM_READ)
      : Machine(Machine), Code(std::move(Code)),
        Characteristics(Characteristics) {}

  void addFunction(StringRef Name, uint32_t Offset,
                   uint8_t Class = COFF::IMAGE_SYM_CLASS_EXTERNAL) {
    Symbols.push_back(
        {Name.str(), Offset, 1,
         COFF::IMAGE_SYM_DTYPE_FUNCTION << COFF::SCT_COMPLEX_TYPE_SHIFT,
         Class});
  }

  void addLabel(StringRef Name, uint32_t Offset) {
    Symbols.push_back({Name.str(), Offset, 1, 0, COFF::IMAGE_SYM_CLASS_LABEL});
  }

  void addRelocation(uint32_t Offset, uint16_t Type) {
    Relocations.push_back({Offset, Type});
  }

  std::vector<uint8_t> build() const {
    std::vector<Symbol> All = Symbols;
    All.push_back(
        {"target", 0, 0,
         COFF::IMAGE_SYM_DTYPE_FUNCTION << COFF::SCT_COMPLEX_TYPE_SHIFT,
         COFF::IMAGE_SYM_CLASS_EXTERNAL});
    // Two slots for the section symbol and its auxiliary record.
    const uint32_t TargetIndex = 2 + static_cast<uint32_t>(Symbols.size());

    const uint32_t HeaderSize = 20 + 40;
    const uint32_t RawOffset = HeaderSize;
    const uint32_t RelocOffset = RawOffset + static_cast<uint32_t>(Code.size());
    const uint32_t SymbolOffset =
        RelocOffset + 10 * static_cast<uint32_t>(Relocations.size());
    const uint32_t SymbolCount = 2 + static_cast<uint32_t>(All.size());

    std::vector<uint8_t> Out;
    auto put16 = [&](uint16_t V) {
      Out.push_back(V & 0xFF);
      Out.push_back(V >> 8);
    };
    auto put32 = [&](uint32_t V) {
      for (int I = 0; I < 4; ++I)
        Out.push_back((V >> (8 * I)) & 0xFF);
    };
    auto putName = [&](StringRef Name, std::string &Strings) {
      uint8_t Field[8] = {};
      if (Name.size() <= 8) {
        std::memcpy(Field, Name.data(), Name.size());
      } else {
        uint32_t Offset = 4 + static_cast<uint32_t>(Strings.size());
        Strings += Name.str();
        Strings.push_back('\0');
        std::memcpy(Field + 4, &Offset, 4);
      }
      Out.insert(Out.end(), Field, Field + 8);
    };

    // File header.
    put16(Machine);
    put16(1);
    put32(0);
    put32(SymbolOffset);
    put32(SymbolCount);
    put16(0);
    put16(0);

    // Section header.
    std::string Strings;
    putName(".text", Strings);
    put32(0);
    put32(0);
    put32(static_cast<uint32_t>(Code.size()));
    put32(RawOffset);
    put32(Relocations.empty() ? 0 : RelocOffset);
    put32(0);
    put16(static_cast<uint16_t>(Relocations.size()));
    put16(0);
    put32(Characteristics);

    Out.insert(Out.end(), Code.begin(), Code.end());

    for (const auto &[Offset, Type] : Relocations) {
      put32(Offset);
      put32(TargetIndex);
      put16(Type);
    }

    // Section symbol and its auxiliary section-definition record.
    putName(".text", Strings);
    put32(0);
    put16(1);
    put16(0);
    Out.push_back(COFF::IMAGE_SYM_CLASS_STATIC);
    Out.push_back(1);
    put32(static_cast<uint32_t>(Code.size()));
    put16(static_cast<uint16_t>(Relocations.size()));
    put16(0);
    put32(0);
    put16(0);
    Out.push_back(0);
    Out.insert(Out.end(), 3, 0);

    for (const Symbol &Sym : All) {
      putName(Sym.Name, Strings);
      put32(Sym.Value);
      put16(static_cast<uint16_t>(Sym.Section));
      put16(Sym.Type);
      Out.push_back(Sym.Class);
      Out.push_back(0);
    }

    put32(4 + static_cast<uint32_t>(Strings.size()));
    Out.insert(Out.end(), Strings.begin(), Strings.end());
    return Out;
  }

private:
  uint16_t Machine;
  std::vector<uint8_t> Code;
  uint32_t Characteristics;
  std::vector<Symbol> Symbols;
  std::vector<std::pair<uint32_t, uint16_t>> Relocations;
};

struct Generated {
  std::vector<std::string> Lines;
  PatternGeneratorStats Stats;
};

Generated generate(const std::vector<uint8_t> &Object,
                   PatternGeneratorOptions Opts = {}) {
  StringRef Bytes(reinterpret_cast<const char *>(Object.data()), Object.size());
  auto ObjOrErr =
      object::ObjectFile::createObjectFile(MemoryBufferRef(Bytes, "test.obj"));
  if (!ObjOrErr) {
    ADD_FAILURE() << toString(ObjOrErr.takeError());
    return {};
  }
  std::string Text;
  raw_string_ostream OS(Text);
  Generated Result;
  Result.Stats = generatePatterns(**ObjOrErr, Opts, OS);
  OS.flush();
  StringRef Rest(Text);
  while (!Rest.empty()) {
    auto [Line, Tail] = Rest.split('\n');
    if (!Line.empty())
      Result.Lines.push_back(Line.str());
    Rest = Tail;
  }
  return Result;
}

std::vector<uint8_t> sequentialCode(size_t Size) {
  std::vector<uint8_t> Code(Size);
  for (size_t I = 0; I < Size; ++I)
    Code[I] = static_cast<uint8_t>(0x40 + I);
  return Code;
}

/// Parses \p Line and checks it against \p Linked, a copy of the function's
/// bytes as a linker would leave them.
bool lineMatches(StringRef Line, const std::vector<uint8_t> &Linked) {
  auto ModOrErr = PatternParser::parseLine(Line);
  if (!ModOrErr) {
    ADD_FAILURE() << toString(ModOrErr.takeError());
    return false;
  }
  return SignatureMatcher::matchPattern(*ModOrErr, Linked.data(),
                                        Linked.size());
}

} // anonymous namespace

TEST(PatternGeneratorCOFF, RelocationWidthsCoverWholeRewrittenFields) {
  using namespace COFF;
  EXPECT_EQ(coffRelocationWidth(IMAGE_FILE_MACHINE_ARMNT, IMAGE_REL_ARM_MOV32T),
            8u);
  EXPECT_EQ(coffRelocationWidth(IMAGE_FILE_MACHINE_ARMNT, IMAGE_REL_ARM_MOV32A),
            8u);
  EXPECT_EQ(coffRelocationWidth(IMAGE_FILE_MACHINE_ARMNT, IMAGE_REL_ARM_BLX23T),
            4u);
  EXPECT_EQ(coffRelocationWidth(IMAGE_FILE_MACHINE_ARMNT, IMAGE_REL_ARM_PAIR),
            0u);
  EXPECT_EQ(coffRelocationWidth(IMAGE_FILE_MACHINE_ARM64,
                                IMAGE_REL_ARM64_PAGEBASE_REL21),
            4u);
  EXPECT_EQ(
      coffRelocationWidth(IMAGE_FILE_MACHINE_ARM64, IMAGE_REL_ARM64_ADDR64),
      8u);
  EXPECT_EQ(
      coffRelocationWidth(IMAGE_FILE_MACHINE_ARM64EC, IMAGE_REL_ARM64_BRANCH26),
      4u);
  EXPECT_EQ(
      coffRelocationWidth(IMAGE_FILE_MACHINE_AMD64, IMAGE_REL_AMD64_ADDR64),
      8u);
  EXPECT_EQ(
      coffRelocationWidth(IMAGE_FILE_MACHINE_AMD64, IMAGE_REL_AMD64_REL32_4),
      4u);
  EXPECT_EQ(
      coffRelocationWidth(IMAGE_FILE_MACHINE_AMD64, IMAGE_REL_AMD64_SECTION),
      2u);
  EXPECT_EQ(coffRelocationWidth(IMAGE_FILE_MACHINE_I386, IMAGE_REL_I386_DIR32),
            4u);
  EXPECT_EQ(coffRelocationWidth(IMAGE_FILE_MACHINE_I386, IMAGE_REL_I386_DIR16),
            2u);
  EXPECT_EQ(coffRelocationWidth(IMAGE_FILE_MACHINE_ARMNT, 0x13), std::nullopt);
  EXPECT_EQ(coffRelocationWidth(IMAGE_FILE_MACHINE_R4000, 1), std::nullopt);
}

TEST(PatternGeneratorCOFF, Mov32TPairIsWildcardedAcrossBothInstructions) {
  std::vector<uint8_t> Code = sequentialCode(64);
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_ARMNT, Code);
  Builder.addFunction("?Run@Task@@QAAXXZ", 0);
  Builder.addFunction("helper", 40, COFF::IMAGE_SYM_CLASS_STATIC);
  Builder.addRelocation(8, COFF::IMAGE_REL_ARM_MOV32T);
  Builder.addRelocation(24, COFF::IMAGE_REL_ARM_BLX23T);
  Builder.addRelocation(44, COFF::IMAGE_REL_ARM_ADDR32);

  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 2u);
  EXPECT_EQ(Out.Stats.Functions, 2u);

  // Bytes 8-15 are the MOVW/MOVT pair, 24-27 the BLX; the CRC covers the
  // eight bytes after the leading 32, and the name is spelled as the
  // symbol table spells it.
  std::string Lead;
  for (unsigned I = 0; I < 32; ++I) {
    bool Relocated = (I >= 8 && I < 16) || (I >= 24 && I < 28);
    char Hex[3];
    std::snprintf(Hex, sizeof(Hex), "%02X", Code[I]);
    Lead += Relocated ? std::string("..") : std::string(Hex);
  }
  EXPECT_EQ(StringRef(Out.Lines[0]).take_front(64), Lead);
  EXPECT_TRUE(StringRef(Out.Lines[0]).contains(" 08 "));
  EXPECT_TRUE(
      StringRef(Out.Lines[0]).ends_with(" 0028 :0000 ?Run@Task@@QAAXXZ"));
  EXPECT_TRUE(StringRef(Out.Lines[1]).ends_with(" 0018 :0000 helper"));
  EXPECT_TRUE(StringRef(Out.Lines[1]).starts_with("68696A6B........"));

  // A linker rewrites every relocated byte; the signature must still match.
  std::vector<uint8_t> Linked(Code.begin(), Code.begin() + 40);
  for (unsigned I : {8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 24u, 25u, 26u, 27u})
    Linked[I] ^= 0xFF;
  EXPECT_TRUE(lineMatches(Out.Lines[0], Linked));

  // Any byte outside a relocation is the function's own.
  for (unsigned I : {0u, 7u, 16u, 23u, 28u, 31u, 32u, 39u}) {
    std::vector<uint8_t> Changed = Linked;
    Changed[I] ^= 0x01;
    EXPECT_FALSE(lineMatches(Out.Lines[0], Changed)) << "byte " << I;
  }
}

TEST(PatternGeneratorCOFF, LabelsDoNotEndAFunction) {
  std::vector<uint8_t> Code = sequentialCode(64);
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
  Builder.addFunction("?Dispatch@@YAHH@Z", 0);
  Builder.addLabel("$LN5", 20);
  Builder.addLabel("$LN7", 36);
  Builder.addFunction("next", 48);
  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;

  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 2u);
  auto Mod = PatternParser::parseLine(Out.Lines[0]);
  ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
  EXPECT_EQ(Mod->TotalLen, 48u);
  EXPECT_TRUE(SignatureMatcher::isFullyVerified(*Mod));
  ASSERT_EQ(Mod->PublicNames.size(), 1u);
  EXPECT_EQ(Mod->PublicNames[0].Name, "?Dispatch@@YAHH@Z");
}

TEST(PatternGeneratorCOFF, DirectBranchesBecomeReferencesOnRequest) {
  // x64: a call at 8 and a RIP-relative load at 16; only the call's rel32
  // field is a branch the matcher can follow.
  std::vector<uint8_t> Code = sequentialCode(48);
  Code[8] = 0xE8;
  Code[16] = 0x8B;
  Code[17] = 0x05;
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
  Builder.addFunction("caller", 0);
  Builder.addRelocation(9, COFF::IMAGE_REL_AMD64_REL32);
  Builder.addRelocation(18, COFF::IMAGE_REL_AMD64_REL32);
  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;

  Generated Plain = generate(Builder.build(), Opts);
  ASSERT_EQ(Plain.Lines.size(), 1u);
  EXPECT_EQ(Plain.Lines[0].find('^'), std::string::npos)
      << "references are off unless asked for";

  Opts.EmitReferences = true;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  auto Mod = PatternParser::parseLine(Out.Lines[0]);
  ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
  ASSERT_EQ(Mod->References.size(), 1u);
  EXPECT_EQ(Mod->References[0].Offset, 9u);
  EXPECT_EQ(Mod->References[0].Name, "target");
}

TEST(PatternGeneratorCOFF, BranchReferencesUseEachMachinesOffset) {
  using namespace COFF;
  const std::vector<uint8_t> Call = {0x55, 0xE8, 0, 0, 0, 0, 0xC3};
  EXPECT_EQ(coffBranchReferenceOffset(IMAGE_FILE_MACHINE_I386,
                                      IMAGE_REL_I386_REL32, Call, 2, 0),
            2u);
  // The same relocation after a non-branch opcode, or at the function's
  // first byte, is no branch.
  EXPECT_EQ(coffBranchReferenceOffset(IMAGE_FILE_MACHINE_I386,
                                      IMAGE_REL_I386_REL32, Call, 3, 0),
            std::nullopt);
  EXPECT_EQ(coffBranchReferenceOffset(IMAGE_FILE_MACHINE_I386,
                                      IMAGE_REL_I386_REL32, Call, 2, 2),
            std::nullopt);
  EXPECT_EQ(coffBranchReferenceOffset(IMAGE_FILE_MACHINE_I386,
                                      IMAGE_REL_I386_DIR32, Call, 2, 0),
            std::nullopt);
  const std::vector<uint8_t> Words(16, 0);
  EXPECT_EQ(coffBranchReferenceOffset(IMAGE_FILE_MACHINE_ARM64,
                                      IMAGE_REL_ARM64_BRANCH26, Words, 12, 4),
            8u);
  EXPECT_EQ(coffBranchReferenceOffset(IMAGE_FILE_MACHINE_ARMNT,
                                      IMAGE_REL_ARM_BLX23T, Words, 6, 2),
            4u);
  EXPECT_EQ(coffBranchReferenceOffset(IMAGE_FILE_MACHINE_ARM64,
                                      IMAGE_REL_ARM64_PAGEBASE_REL21, Words, 12,
                                      4),
            std::nullopt);
}

TEST(PatternGeneratorCOFF, Addr64IsWildcardedAcrossEightBytes) {
  // sub rsp, 28h ; mov rax, imm64 (the address) ; call rel32 ;
  // add rsp, 28h ; xor eax, eax ; ret ; int3 padding
  std::vector<uint8_t> Code = {0x48, 0x83, 0xEC, 0x28, 0x48, 0xB8, 1,    2,
                               3,    4,    5,    6,    7,    8,    0xE8, 9,
                               10,   11,   12,   0x48, 0x83, 0xC4, 0x28, 0x33,
                               0xC0, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
  Builder.addFunction("load_address", 0);
  Builder.addRelocation(6, COFF::IMAGE_REL_AMD64_ADDR64);
  Builder.addRelocation(15, COFF::IMAGE_REL_AMD64_REL32);

  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_EQ(Out.Lines[0], "4883EC2848B8................E8........4883C4283"
                          "3C0C3CCCCCCCCCCCC 00 0000 0020 :0000 load_address");
}

TEST(PatternGeneratorCOFF, UnknownRelocationLeavesOnlyItsFunctionOut) {
  std::vector<uint8_t> Code = sequentialCode(32);
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_ARMNT, Code);
  Builder.addFunction("unknown_reloc", 0);
  Builder.addFunction("clean", 16);
  Builder.addRelocation(4, 0x13);

  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).ends_with(":0000 clean"));
  EXPECT_EQ(Out.Stats.UnsupportedRelocation, 1u);
  EXPECT_EQ(Out.Stats.UnsupportedCOFFRelocations.count(
                {COFF::IMAGE_FILE_MACHINE_ARMNT, 0x13}),
            1u);
}

TEST(PatternGeneratorCOFF, DataSectionFunctionSymbolsAreNotFunctions) {
  std::vector<uint8_t> Data = sequentialCode(32);
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_I386, Data,
                            COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
                                COFF::IMAGE_SCN_MEM_READ);
  Builder.addFunction("_table", 0);
  Generated Out = generate(Builder.build());
  EXPECT_TRUE(Out.Lines.empty());
  EXPECT_EQ(Out.Stats.Functions, 0u);
}

TEST(PatternGeneratorCOFF, ShortFunctionsAreCountedNotWritten) {
  std::vector<uint8_t> Code = {0x33, 0xC0, 0xC3, 0xCC};
  std::vector<uint8_t> Frame = sequentialCode(20);
  Code.insert(Code.end(), Frame.begin(), Frame.end());
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_I386, Code);
  Builder.addFunction("_zero", 0);
  Builder.addFunction("_frame", 4);
  PatternGeneratorOptions Opts;
  Opts.MinFuncSize = 5;

  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).ends_with(" 0014 :0000 _frame"));
  EXPECT_EQ(Out.Stats.TooSmall, 1u);
}

TEST(PatternGeneratorCOFF, LinesStatingTooFewExactBytesAreNotWritten) {
  std::vector<uint8_t> Code = sequentialCode(28 + 12 + 32);
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_ARM64, Code);
  // Every instruction relocated: the line would state nothing at all.
  Builder.addFunction("?RmDirRecursive@fuzzer@@YAXXZ", 0);
  for (uint32_t Offset = 0; Offset < 28; Offset += 4)
    Builder.addRelocation(Offset, COFF::IMAGE_REL_ARM64_BRANCH26);
  // Eight exact bytes then a relocated instruction: below the floor.
  Builder.addFunction("eight", 28);
  Builder.addRelocation(28 + 8, COFF::IMAGE_REL_ARM64_BRANCH26);
  // Sixteen exact bytes, a relocated instruction, and twelve more: enough.
  Builder.addFunction("sixteen", 40);
  Builder.addRelocation(40 + 16, COFF::IMAGE_REL_ARM64_BRANCH26);

  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).ends_with(":0000 sixteen"));
  EXPECT_EQ(Out.Stats.TooWeak, 2u);

  SmallVector<bool> AllRelocated(28, true);
  EXPECT_EQ(statedByteCount(AllRelocated, Opts), 0u);
  SmallVector<bool> Floor(20, false);
  Floor[16] = Floor[17] = Floor[18] = Floor[19] = true;
  EXPECT_EQ(statedByteCount(Floor, Opts), SignatureMatcher::MinStatedBytes);
}
