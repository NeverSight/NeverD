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
#include "llvm/BinaryFormat/ELF.h"
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

/// A relocatable ELF object with one code section, assembled in memory.
///
/// ELFCLASS64 objects carry RELA relocations and ELFCLASS32 ones REL, as the
/// x86-64 and AArch64, and the i386 and ARM, toolchains write them. Every
/// function is a global STT_FUNC symbol in .text; every relocation points at
/// one undefined symbol, added by build().
class ELFObjectBuilder {
public:
  ELFObjectBuilder(uint16_t Machine, bool Is64, std::vector<uint8_t> Code)
      : Machine(Machine), Is64(Is64), Code(std::move(Code)) {}

  void addFunction(StringRef Name, uint64_t Offset) {
    Functions.push_back({Name.str(), Offset});
  }

  void addRelocation(uint64_t Offset, uint32_t Type) {
    Relocations.push_back({Offset, Type});
  }

  std::vector<uint8_t> build() const {
    std::vector<uint8_t> Out;
    auto put = [&](uint64_t V, unsigned Bytes) {
      for (unsigned I = 0; I < Bytes; ++I)
        Out.push_back((V >> (8 * I)) & 0xFF);
    };
    const unsigned Addr = Is64 ? 8 : 4;
    auto align = [&](size_t To) {
      while (Out.size() % To)
        Out.push_back(0);
    };

    std::string Strings(1, '\0');
    std::vector<uint32_t> NameOffsets;
    for (const auto &[Name, Offset] : Functions) {
      NameOffsets.push_back(static_cast<uint32_t>(Strings.size()));
      Strings += Name;
      Strings.push_back('\0');
    }
    const uint32_t TargetName = static_cast<uint32_t>(Strings.size());
    Strings += "target";
    Strings.push_back('\0');
    const uint32_t TargetIndex = 1 + static_cast<uint32_t>(Functions.size());
    const char *RelName = Is64 ? ".rela.text" : ".rel.text";
    std::string SectionNames(1, '\0');
    auto addName = [&](StringRef Name) {
      uint32_t Offset = static_cast<uint32_t>(SectionNames.size());
      SectionNames += Name.str();
      SectionNames.push_back('\0');
      return Offset;
    };
    const uint32_t TextName = addName(".text");
    const uint32_t RelSectionName = addName(RelName);
    const uint32_t SymtabName = addName(".symtab");
    const uint32_t StrtabName = addName(".strtab");
    const uint32_t ShstrtabName = addName(".shstrtab");

    const size_t HeaderSize = Is64 ? 64 : 52;
    Out.resize(HeaderSize);

    struct Placed {
      uint64_t Offset, Size;
    };
    align(16);
    const Placed Text{Out.size(), Code.size()};
    Out.insert(Out.end(), Code.begin(), Code.end());

    align(8);
    const size_t RelBegin = Out.size();
    for (const auto &[Offset, Type] : Relocations) {
      put(Offset, Addr);
      if (Is64) {
        put((uint64_t(TargetIndex) << 32) | Type, 8);
        put(0, 8);
      } else {
        put((TargetIndex << 8) | Type, 4);
      }
    }
    const Placed Rel{RelBegin, Out.size() - RelBegin};

    align(8);
    const size_t SymBegin = Out.size();
    auto putSymbol = [&](uint32_t Name, uint8_t Info, uint16_t Section,
                         uint64_t Value) {
      put(Name, 4);
      if (Is64) {
        Out.push_back(Info);
        Out.push_back(0);
        put(Section, 2);
        put(Value, 8);
        put(0, 8);
      } else {
        put(Value, 4);
        put(0, 4);
        Out.push_back(Info);
        Out.push_back(0);
        put(Section, 2);
      }
    };
    putSymbol(0, 0, 0, 0);
    for (size_t I = 0; I < Functions.size(); ++I)
      putSymbol(NameOffsets[I], (ELF::STB_GLOBAL << 4) | ELF::STT_FUNC, 1,
                Functions[I].second);
    putSymbol(TargetName, (ELF::STB_GLOBAL << 4) | ELF::STT_NOTYPE,
              ELF::SHN_UNDEF, 0);
    const Placed Symtab{SymBegin, Out.size() - SymBegin};

    const Placed Strtab{Out.size(), Strings.size()};
    Out.insert(Out.end(), Strings.begin(), Strings.end());
    const Placed Shstrtab{Out.size(), SectionNames.size()};
    Out.insert(Out.end(), SectionNames.begin(), SectionNames.end());

    align(8);
    const uint64_t SectionHeaders = Out.size();
    auto putSection = [&](uint32_t Name, uint32_t Type, uint64_t Flags,
                          Placed Where, uint32_t Link, uint32_t Info,
                          uint64_t Align, uint64_t EntSize) {
      put(Name, 4);
      put(Type, 4);
      put(Flags, Addr);
      put(0, Addr);
      put(Where.Offset, Addr);
      put(Where.Size, Addr);
      put(Link, 4);
      put(Info, 4);
      put(Align, Addr);
      put(EntSize, Addr);
    };
    putSection(0, ELF::SHT_NULL, 0, {0, 0}, 0, 0, 0, 0);
    putSection(TextName, ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_EXECINSTR,
               Text, 0, 0, 16, 0);
    putSection(RelSectionName, Is64 ? ELF::SHT_RELA : ELF::SHT_REL,
               ELF::SHF_INFO_LINK, Rel, 3, 1, Addr, Is64 ? 24 : 8);
    putSection(SymtabName, ELF::SHT_SYMTAB, 0, Symtab, 4, 1, Addr,
               Is64 ? 24 : 16);
    putSection(StrtabName, ELF::SHT_STRTAB, 0, Strtab, 0, 0, 1, 0);
    putSection(ShstrtabName, ELF::SHT_STRTAB, 0, Shstrtab, 0, 0, 1, 0);

    std::vector<uint8_t> Header;
    std::swap(Header, Out);
    Out.clear();
    const uint8_t Ident[16] = {0x7F, 'E', 'L', 'F',
                               uint8_t(Is64 ? ELF::ELFCLASS64 : ELF::ELFCLASS32),
                               ELF::ELFDATA2LSB, ELF::EV_CURRENT};
    Out.insert(Out.end(), Ident, Ident + 16);
    put(ELF::ET_REL, 2);
    put(Machine, 2);
    put(ELF::EV_CURRENT, 4);
    put(0, Addr);
    put(0, Addr);
    put(SectionHeaders, Addr);
    put(0, 4);
    put(HeaderSize, 2);
    put(0, 2);
    put(0, 2);
    put(Is64 ? 64 : 40, 2);
    put(6, 2);
    put(5, 2);
    std::copy(Out.begin(), Out.end(), Header.begin());
    return Header;
  }

private:
  uint16_t Machine;
  bool Is64;
  std::vector<uint8_t> Code;
  std::vector<std::pair<std::string, uint64_t>> Functions;
  std::vector<std::pair<uint64_t, uint32_t>> Relocations;
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

/// \p Code with \p Replacement written over it at \p Offset: the bytes a
/// linker leaves where the object held placeholders.
std::vector<uint8_t> linked(std::vector<uint8_t> Code, size_t Offset,
                            std::vector<uint8_t> Replacement) {
  std::copy(Replacement.begin(), Replacement.end(), Code.begin() + Offset);
  return Code;
}

TEST(PatternGeneratorELF, RelocationSectionsApplyToTheCodeTheyName) {
  // push rbp; mov rbp,rsp; call target (PLT32); mov rbp,[rip+stdout] (PC32)
  std::vector<uint8_t> Code = {0x55, 0x48, 0x89, 0xE5, 0xE8, 0,    0,    0,
                               0,    0x48, 0x8B, 0x2D, 0,    0,    0,    0};
  std::vector<uint8_t> Rest = sequentialCode(16);
  Code.insert(Code.end(), Rest.begin(), Rest.end());
  ELFObjectBuilder Builder(ELF::EM_X86_64, true, Code);
  Builder.addFunction("_IO_puts", 0);
  Builder.addRelocation(5, ELF::R_X86_64_PLT32);
  Builder.addRelocation(12, ELF::R_X86_64_PC32);

  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).starts_with("554889E5E8........488B2D........"));
  std::vector<uint8_t> Linked =
      linked(linked(Code, 5, {0x16, 0xFE, 0xFF, 0xFF}), 12, {0x5D, 0x3C, 0x1A, 0});
  EXPECT_TRUE(lineMatches(Out.Lines[0], Linked));
}

TEST(PatternGeneratorELF, RelaxedGotAccessesKeepTheirMatch) {
  // mov rax,[rip+foo@GOTPCREL]; mov eax,[rip+bar@GOTPCREL];
  // call [rip+baz@GOTPCREL]; then code no relocation touches.
  std::vector<uint8_t> Code = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x8B, 0x05, 0, 0,
                               0,    0,    0xFF, 0x15, 0, 0, 0, 0};
  std::vector<uint8_t> Rest = sequentialCode(20);
  Code.insert(Code.end(), Rest.begin(), Rest.end());
  ELFObjectBuilder Builder(ELF::EM_X86_64, true, Code);
  Builder.addFunction("gotloads", 0);
  Builder.addRelocation(3, ELF::R_X86_64_REX_GOTPCRELX);
  Builder.addRelocation(9, ELF::R_X86_64_GOTPCRELX);
  Builder.addRelocation(15, ELF::R_X86_64_GOTPCRELX);

  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  // In a static link: lea rax,[rip+foo]; mov eax,imm32; addr32 call baz.
  std::vector<uint8_t> Relaxed = linked(
      Code, 0,
      {0x48, 0x8D, 0x05, 0x10, 0x20, 0, 0, 0xC7, 0xC0, 0x30, 0x40, 0x50, 0,
       0x67, 0xE8, 0x60, 0x70, 0, 0});
  EXPECT_TRUE(lineMatches(Out.Lines[0], Relaxed));
}

TEST(PatternGeneratorELF, TlsSequencesAreWildcardedWhole) {
  // General dynamic: data16 lea rdi,[rip+x@tlsgd]; data16 data16 rex64 call
  // __tls_get_addr@PLT.
  std::vector<uint8_t> Code = {0x66, 0x48, 0x8D, 0x3D, 0,    0, 0, 0,
                               0x66, 0x66, 0x48, 0xE8, 0,    0, 0, 0};
  std::vector<uint8_t> Rest = sequentialCode(24);
  Code.insert(Code.end(), Rest.begin(), Rest.end());
  ELFObjectBuilder Builder(ELF::EM_X86_64, true, Code);
  Builder.addFunction("tls_reader", 0);
  Builder.addRelocation(4, ELF::R_X86_64_TLSGD);
  Builder.addRelocation(12, ELF::R_X86_64_PLT32);

  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  // Local exec: mov rax,fs:0; lea rax,[rax+x@tpoff].
  std::vector<uint8_t> Relaxed =
      linked(Code, 0,
             {0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0, 0x48, 0x8D, 0x80, 0xF8,
              0xFF, 0xFF, 0xFF});
  EXPECT_TRUE(lineMatches(Out.Lines[0], Relaxed));
}

TEST(PatternGeneratorELF, WideAndNarrowFieldsHaveTheirOwnWidths) {
  // movabs rax,imm64 (R_X86_64_64) then a byte that stays stated.
  std::vector<uint8_t> Code = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3};
  std::vector<uint8_t> Rest = sequentialCode(24);
  Code.insert(Code.end(), Rest.begin(), Rest.end());
  ELFObjectBuilder Builder(ELF::EM_X86_64, true, Code);
  Builder.addFunction("load_address", 0);
  Builder.addRelocation(2, ELF::R_X86_64_64);

  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).starts_with("48B8................C3"));
}

TEST(PatternGeneratorELF, I386GotLoadsAreRelaxable) {
  // mov eax,[ebx+foo@GOT] becomes lea eax,[ebx+foo@GOTOFF].
  std::vector<uint8_t> Code = {0x8B, 0x83, 0, 0, 0, 0};
  std::vector<uint8_t> Rest = sequentialCode(24);
  Code.insert(Code.end(), Rest.begin(), Rest.end());
  ELFObjectBuilder Builder(ELF::EM_386, false, Code);
  Builder.addFunction("got_load", 0);
  Builder.addRelocation(2, ELF::R_386_GOT32X);

  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(lineMatches(Out.Lines[0],
                          linked(Code, 0, {0x8D, 0x83, 0x10, 0xE0, 0xFF, 0xFF})));
}

TEST(PatternGeneratorELF, AArch64RelocationsCoverTheirInstruction) {
  std::vector<uint8_t> Code = sequentialCode(8);
  const std::vector<uint8_t> Bl = {0, 0, 0, 0x94};
  Code.insert(Code.end(), Bl.begin(), Bl.end());
  std::vector<uint8_t> Rest = sequentialCode(20);
  Code.insert(Code.end(), Rest.begin(), Rest.end());
  ELFObjectBuilder Builder(ELF::EM_AARCH64, true, Code);
  Builder.addFunction("caller", 0);
  Builder.addRelocation(8, ELF::R_AARCH64_CALL26);

  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).starts_with("4041424344454647........"));
}

TEST(PatternGeneratorELF, UnknownRelocationLeavesOnlyItsFunctionOut) {
  std::vector<uint8_t> Code = sequentialCode(64);
  ELFObjectBuilder Builder(ELF::EM_X86_64, true, Code);
  Builder.addFunction("copied", 0);
  Builder.addFunction("clean", 32);
  Builder.addRelocation(4, ELF::R_X86_64_COPY);

  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).ends_with(":0000 clean"));
  EXPECT_EQ(Out.Stats.UnsupportedRelocation, 1u);
  EXPECT_EQ(Out.Stats.UnsupportedELFRelocations.count(
                {ELF::EM_X86_64, ELF::R_X86_64_COPY}),
            1u);
}

TEST(PatternGeneratorELF, FootprintTable) {
  auto Is = [](uint16_t Machine, uint32_t Type, unsigned Before,
               unsigned Width) {
    std::optional<ELFRelocationFootprint> F =
        elfRelocationFootprint(Machine, Type);
    return F && F->Before == Before && F->Width == Width;
  };
  EXPECT_TRUE(Is(ELF::EM_X86_64, ELF::R_X86_64_TLSLD, 3, 10));
  EXPECT_TRUE(Is(ELF::EM_X86_64, ELF::R_X86_64_TLSDESC_CALL, 0, 2));
  EXPECT_TRUE(Is(ELF::EM_X86_64, ELF::R_X86_64_CODE_4_GOTPCRELX, 4, 4));
  EXPECT_TRUE(Is(ELF::EM_386, ELF::R_386_TLS_GD, 3, 10));
  EXPECT_TRUE(Is(ELF::EM_AARCH64, ELF::R_AARCH64_TLSDESC_CALL, 0, 4));
  EXPECT_TRUE(Is(ELF::EM_AARCH64, ELF::R_AARCH64_PREL64, 0, 8));
  EXPECT_TRUE(Is(ELF::EM_ARM, ELF::R_ARM_THM_JUMP11, 0, 2));
  EXPECT_TRUE(Is(ELF::EM_ARM, ELF::R_ARM_THM_TLS_DESCSEQ32, 0, 4));
  EXPECT_TRUE(Is(ELF::EM_ARM, ELF::R_ARM_V4BX, 0, 4));
  // Dynamic types, private ones, and machines the table does not know.
  EXPECT_FALSE(elfRelocationFootprint(ELF::EM_X86_64, ELF::R_X86_64_JUMP_SLOT));
  EXPECT_FALSE(elfRelocationFootprint(ELF::EM_AARCH64, ELF::R_AARCH64_COPY));
  EXPECT_FALSE(elfRelocationFootprint(ELF::EM_ARM, ELF::R_ARM_PRIVATE_0));
  EXPECT_FALSE(elfRelocationFootprint(ELF::EM_MIPS, ELF::R_MIPS_32));
}

TEST(PatternGeneratorELF, SymbolsThatLabelOneAddressShareALine) {
  std::vector<uint8_t> Code = sequentialCode(64);
  ELFObjectBuilder Builder(ELF::EM_X86_64, true, Code);
  // glibc defines _IO_puts and makes puts an alias of it.
  Builder.addFunction("_IO_puts", 0);
  Builder.addFunction("puts", 0);
  Builder.addFunction("__IO_puts_internal", 0);
  Builder.addFunction("fputs", 32);

  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 2u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).ends_with(
      ":0000 puts :0000 _IO_puts :0000 __IO_puts_internal"))
      << Out.Lines[0];
  EXPECT_TRUE(StringRef(Out.Lines[1]).ends_with(":0000 fputs"));
  EXPECT_EQ(Out.Stats.Functions, 2u);
  auto Parsed = PatternParser::parseLine(Out.Lines[0]);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  EXPECT_EQ(Parsed->PublicNames.size(), 3u);
}

TEST(PatternGeneratorMachine, ObjectsAreKeptForTheirOwnArchitecture) {
  auto Is = [](const std::vector<uint8_t> &Bytes, TargetMachine Machine) {
    StringRef Data(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    auto ObjOrErr =
        object::ObjectFile::createObjectFile(MemoryBufferRef(Data, "test.o"));
    if (!ObjOrErr) {
      ADD_FAILURE() << toString(ObjOrErr.takeError());
      return false;
    }
    return isObjectForMachine(**ObjOrErr, Machine);
  };
  const std::vector<uint8_t> Code = sequentialCode(32);
  const auto X64 = ELFObjectBuilder(ELF::EM_X86_64, true, Code).build();
  const auto I386 = ELFObjectBuilder(ELF::EM_386, false, Code).build();
  // The x32 ABI: x86-64 instructions in an ELFCLASS32 object.
  const auto X32 = ELFObjectBuilder(ELF::EM_X86_64, false, Code).build();
  const auto AArch64 = ELFObjectBuilder(ELF::EM_AARCH64, true, Code).build();
  const auto Amd64COFF =
      COFFObjectBuilder(COFF::IMAGE_FILE_MACHINE_AMD64, Code).build();

  EXPECT_TRUE(Is(X64, TargetMachine::X64));
  EXPECT_FALSE(Is(X64, TargetMachine::X86));
  EXPECT_TRUE(Is(I386, TargetMachine::X86));
  EXPECT_FALSE(Is(I386, TargetMachine::X64));
  EXPECT_FALSE(Is(X32, TargetMachine::X64));
  EXPECT_FALSE(Is(X32, TargetMachine::X86));
  EXPECT_TRUE(Is(AArch64, TargetMachine::ARM64));
  EXPECT_FALSE(Is(AArch64, TargetMachine::ARM));
  EXPECT_TRUE(Is(Amd64COFF, TargetMachine::X64));
  EXPECT_FALSE(Is(Amd64COFF, TargetMachine::ARM64));
  EXPECT_EQ(parseTargetMachine("arm64"), TargetMachine::ARM64);
  EXPECT_FALSE(parseTargetMachine("mips").has_value());
}
