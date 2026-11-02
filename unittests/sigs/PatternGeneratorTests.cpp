//===- PatternGeneratorTests.cpp - .pat generation tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternGenerator.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ObjectFile.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
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

  /// Linker directives, which build() writes to a `.drectve` section.
  void setDirectives(StringRef Text) { Directives = Text.str(); }

  void setRelocationTarget(StringRef Name) { TargetName = Name.str(); }
  void setDataTarget() { TargetIsFunction = false; }
  void setTargetStorage(uint8_t Class, int16_t Section = 0) {
    TargetClass = Class;
    TargetSection = Section;
  }

  std::vector<uint8_t> build() const {
    std::vector<Symbol> All = Symbols;
    All.push_back(
        {TargetName, 0, TargetSection,
         uint16_t(TargetIsFunction ? COFF::IMAGE_SYM_DTYPE_FUNCTION
                                         << COFF::SCT_COMPLEX_TYPE_SHIFT
                                   : 0),
         TargetClass});
    // Two slots for the section symbol and its auxiliary record.
    const uint32_t TargetIndex = 2 + static_cast<uint32_t>(Symbols.size());

    const uint16_t SectionCount = Directives.empty() ? 1 : 2;
    const uint32_t HeaderSize = 20 + 40 * SectionCount;
    const uint32_t RawOffset = HeaderSize;
    const uint32_t RelocOffset = RawOffset + static_cast<uint32_t>(Code.size());
    const uint32_t DirectiveOffset =
        RelocOffset + 10 * static_cast<uint32_t>(Relocations.size());
    const uint32_t SymbolOffset =
        DirectiveOffset + static_cast<uint32_t>(Directives.size());
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
    put16(SectionCount);
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
    if (!Directives.empty()) {
      putName(".drectve", Strings);
      put32(0);
      put32(0);
      put32(static_cast<uint32_t>(Directives.size()));
      put32(DirectiveOffset);
      put32(0);
      put32(0);
      put16(0);
      put16(0);
      put32(COFF::IMAGE_SCN_LNK_INFO | COFF::IMAGE_SCN_LNK_REMOVE |
            COFF::IMAGE_SCN_ALIGN_1BYTES);
    }

    Out.insert(Out.end(), Code.begin(), Code.end());

    for (const auto &[Offset, Type] : Relocations) {
      put32(Offset);
      put32(TargetIndex);
      put16(Type);
    }
    Out.insert(Out.end(), Directives.begin(), Directives.end());

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
  std::string Directives;
  std::string TargetName = "target";
  bool TargetIsFunction = true;
  uint8_t TargetClass = COFF::IMAGE_SYM_CLASS_EXTERNAL;
  int16_t TargetSection = 0;
};

/// A relocatable ELF object with one code section, assembled in memory.
///
/// ELFCLASS64 objects carry RELA relocations and ELFCLASS32 ones REL, as the
/// x86-64 and AArch64, and the i386 and ARM, toolchains write them. Symbol 1
/// is the section symbol of .text; every function is a global STT_FUNC
/// symbol in it after that. A relocation names one of the functions, the
/// section symbol (as SectionSymbol), or an undefined symbol build() adds --
/// "target" unless it says otherwise.
class ELFObjectBuilder {
public:
  static constexpr const char *SectionSymbol = ".text";

  ELFObjectBuilder(uint16_t Machine, bool Is64, std::vector<uint8_t> Code)
      : Machine(Machine), Is64(Is64), Code(std::move(Code)) {}

  void addFunction(StringRef Name, uint64_t Offset, uint64_t Size = 0) {
    Functions.push_back({Name.str(), Offset});
    Sizes.push_back(Size);
  }

  void addRelocation(uint64_t Offset, uint32_t Type,
                     StringRef Symbol = "target") {
    Relocations.push_back({Offset, Type, Symbol.str()});
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
    auto addString = [&](StringRef Name) {
      const uint32_t Offset = static_cast<uint32_t>(Strings.size());
      Strings += Name.str();
      Strings.push_back('\0');
      return Offset;
    };
    std::vector<uint32_t> NameOffsets;
    for (const auto &[Name, Offset] : Functions)
      NameOffsets.push_back(addString(Name));
    auto functionIndex = [&](StringRef Name) -> std::optional<uint32_t> {
      for (size_t I = 0; I < Functions.size(); ++I)
        if (Functions[I].first == Name)
          return 2 + static_cast<uint32_t>(I);
      return std::nullopt;
    };
    std::vector<std::string> Undefined = {"target"};
    for (const Relocation &Rel : Relocations)
      if (Rel.Symbol != SectionSymbol && !functionIndex(Rel.Symbol) &&
          !llvm::is_contained(Undefined, Rel.Symbol))
        Undefined.push_back(Rel.Symbol);
    std::vector<uint32_t> UndefinedNames;
    for (const std::string &Name : Undefined)
      UndefinedNames.push_back(addString(Name));
    auto symbolIndex = [&](StringRef Name) -> uint32_t {
      if (Name == SectionSymbol)
        return 1;
      if (std::optional<uint32_t> Index = functionIndex(Name))
        return *Index;
      return 2 + static_cast<uint32_t>(Functions.size()) +
             static_cast<uint32_t>(llvm::find(Undefined, Name) -
                                   Undefined.begin());
    };
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
    for (const Relocation &Rel : Relocations) {
      const uint32_t Index = symbolIndex(Rel.Symbol);
      put(Rel.Offset, Addr);
      if (Is64) {
        put((uint64_t(Index) << 32) | Rel.Type, 8);
        put(0, 8);
      } else {
        put((Index << 8) | Rel.Type, 4);
      }
    }
    const Placed Rel{RelBegin, Out.size() - RelBegin};

    align(8);
    const size_t SymBegin = Out.size();
    auto putSymbol = [&](uint32_t Name, uint8_t Info, uint16_t Section,
                         uint64_t Value, uint64_t Size) {
      put(Name, 4);
      if (Is64) {
        Out.push_back(Info);
        Out.push_back(0);
        put(Section, 2);
        put(Value, 8);
        put(Size, 8);
      } else {
        put(Value, 4);
        put(Size, 4);
        Out.push_back(Info);
        Out.push_back(0);
        put(Section, 2);
      }
    };
    putSymbol(0, 0, 0, 0, 0);
    putSymbol(0, (ELF::STB_LOCAL << 4) | ELF::STT_SECTION, 1, 0, 0);
    for (size_t I = 0; I < Functions.size(); ++I)
      putSymbol(NameOffsets[I], (ELF::STB_GLOBAL << 4) | ELF::STT_FUNC, 1,
                Functions[I].second, Sizes[I]);
    for (uint32_t Name : UndefinedNames)
      putSymbol(Name, (ELF::STB_GLOBAL << 4) | ELF::STT_NOTYPE, ELF::SHN_UNDEF,
                0, 0);
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
    putSection(SymtabName, ELF::SHT_SYMTAB, 0, Symtab, 4, 2, Addr,
               Is64 ? 24 : 16);
    putSection(StrtabName, ELF::SHT_STRTAB, 0, Strtab, 0, 0, 1, 0);
    putSection(ShstrtabName, ELF::SHT_STRTAB, 0, Shstrtab, 0, 0, 1, 0);

    std::vector<uint8_t> Header;
    std::swap(Header, Out);
    Out.clear();
    const uint8_t Ident[16] = {
        0x7F,
        'E',
        'L',
        'F',
        uint8_t(Is64 ? ELF::ELFCLASS64 : ELF::ELFCLASS32),
        ELF::ELFDATA2LSB,
        ELF::EV_CURRENT};
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
  struct Relocation {
    uint64_t Offset;
    uint32_t Type;
    std::string Symbol;
  };
  std::vector<std::pair<std::string, uint64_t>> Functions;
  std::vector<uint64_t> Sizes;
  std::vector<Relocation> Relocations;
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

TEST(PatternGeneratorCOFF, SymbolsAtOneAddressRetainTheirAliases) {
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, sequentialCode(64));
  Builder.addFunction("__copy", 0);
  Builder.addFunction("move", 0);
  Builder.addFunction("copy", 0);
  Builder.addFunction("copy", 0);
  Builder.addLabel("$LN5", 16);
  Builder.addFunction("next", 32);

  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 2u);
  EXPECT_EQ(Out.Stats.Functions, 2u);
  auto Mod = PatternParser::parseLine(Out.Lines[0]);
  ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
  EXPECT_EQ(Mod->TotalLen, 32u);
  ASSERT_EQ(Mod->PublicNames.size(), 3u);
  EXPECT_EQ(Mod->PublicNames[0].Name, "copy");
  EXPECT_EQ(Mod->PublicNames[1].Name, "move");
  EXPECT_EQ(Mod->PublicNames[2].Name, "__copy");
  for (const auto &Name : Mod->PublicNames)
    EXPECT_EQ(Name.Offset, 0u);
}

TEST(PatternGeneratorCOFF, ConditionalTailReferencesNeedWholeFunctionTargets) {
  for (uint16_t Machine :
       {COFF::IMAGE_FILE_MACHINE_I386, COFF::IMAGE_FILE_MACHINE_AMD64}) {
    const uint16_t Type = Machine == COFF::IMAGE_FILE_MACHINE_I386
                              ? uint16_t(COFF::IMAGE_REL_I386_REL32)
                              : uint16_t(COFF::IMAGE_REL_AMD64_REL32);
    for (uint8_t Condition = 0x80; Condition <= 0x8f; ++Condition) {
      std::vector<uint8_t> Code(32, 0x90);
      Code[4] = 0x0f;
      Code[5] = Condition;
      std::fill(Code.begin() + 6, Code.begin() + 10, 0);
      COFFObjectBuilder Builder(Machine, Code);
      Builder.addFunction("dispatcher", 0);
      Builder.setRelocationTarget("short_implementation");
      Builder.addRelocation(6, Type);
      PatternGeneratorOptions Opts;
      Opts.EmitReferences = true;
      auto Out = generate(Builder.build(), Opts);
      ASSERT_EQ(Out.Lines.size(), 1u);
      auto Mod = PatternParser::parseLine(Out.Lines.front());
      ASSERT_TRUE(bool(Mod)) << toString(Mod.takeError());
      ASSERT_EQ(Mod->References.size(), 1u);
      EXPECT_EQ(Mod->References.front().Offset, 6u);
      EXPECT_EQ(Mod->References.front().Name, "short_implementation");
      Builder.setDataTarget();
      Out = generate(Builder.build(), Opts);
      Mod = PatternParser::parseLine(Out.Lines.front());
      ASSERT_TRUE(bool(Mod)) << toString(Mod.takeError());
      EXPECT_TRUE(Mod->References.empty());
      Code[6] = 1; // symbol+1 is not the function entry.
      EXPECT_EQ(coffBranchReferenceOffset(Machine, Type, Code, 6, 0),
                std::nullopt);
    }
    for (uint8_t Prefix : {0x66, 0x67, 0x2e, 0xf2, 0xf3}) {
      const uint8_t Code[] = {Prefix, 0x0f, 0x85, 0, 0, 0, 0};
      EXPECT_EQ(coffBranchReferenceOffset(Machine, Type, Code, 3, 0),
                std::nullopt);
    }
    const uint8_t Code[] = {0x0f, 0x85, 0, 0, 0, 0};
    for (size_t Size = 2; Size < std::size(Code); ++Size)
      EXPECT_EQ(coffBranchReferenceOffset(
                    Machine, Type, ArrayRef(Code).take_front(Size), 2, 0),
                std::nullopt);
    EXPECT_EQ(coffBranchReferenceOffset(Machine, COFF::IMAGE_REL_I386_DIR32,
                                        Code, 2, 0),
              std::nullopt);
  }
}

TEST(PatternGeneratorCOFF, CanonicalIndirectImportsRetainTheirReferences) {
  for (const uint16_t Machine :
       {COFF::IMAGE_FILE_MACHINE_I386, COFF::IMAGE_FILE_MACHINE_AMD64}) {
    for (const uint8_t ModRM : {0x15, 0x25}) {
      std::vector<uint8_t> Code(32, 0x90);
      Code[4] = 0xff;
      Code[5] = ModRM;
      COFFObjectBuilder Builder(Machine, Code);
      Builder.addFunction("caller", 0);
      Builder.setRelocationTarget("__imp_RuntimeAPI");
      const uint16_t Type = Machine == COFF::IMAGE_FILE_MACHINE_I386
                                ? uint16_t(COFF::IMAGE_REL_I386_DIR32)
                                : uint16_t(COFF::IMAGE_REL_AMD64_REL32);
      Builder.addRelocation(6, Type);
      PatternGeneratorOptions Opts;
      Opts.EmitReferences = true;
      Generated Out = generate(Builder.build(), Opts);
      ASSERT_EQ(Out.Lines.size(), 1u);
      auto Mod = PatternParser::parseLine(Out.Lines.front());
      ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
      ASSERT_EQ(Mod->References.size(), 1u);
      EXPECT_EQ(Mod->References.front().Name, "__imp_RuntimeAPI");
      EXPECT_EQ(Mod->References.front().Offset, 6u);
      EXPECT_TRUE(SignatureMatcher::isFullyVerified(*Mod));
      Builder.setRelocationTarget("global_callback");
      Out = generate(Builder.build(), Opts);
      Mod = PatternParser::parseLine(Out.Lines.front());
      ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
      EXPECT_TRUE(Mod->References.empty());
    }
  }
}

TEST(PatternGeneratorCOFF, PrefixedOrTruncatedImportFieldsAreNotGuessed) {
  for (const uint8_t Prefix : {0xf0, 0xf2, 0xf3, 0x2e, 0x36, 0x3e, 0x26, 0x64,
                               0x65, 0x66, 0x67, 0x49, 0x4c, 0x4f}) {
    const std::vector<uint8_t> Code{Prefix, 0xff, 0x15, 0, 0, 0, 0};
    EXPECT_EQ(coffBranchReferenceOffset(COFF::IMAGE_FILE_MACHINE_AMD64,
                                        COFF::IMAGE_REL_AMD64_REL32, Code, 3,
                                        0),
              std::nullopt);
  }
  const std::vector<uint8_t> Code{0xff, 0x15, 0, 0, 0, 0};
  for (size_t Size = 2; Size < Code.size(); ++Size)
    EXPECT_EQ(coffBranchReferenceOffset(COFF::IMAGE_FILE_MACHINE_AMD64,
                                        COFF::IMAGE_REL_AMD64_REL32,
                                        ArrayRef(Code).take_front(Size), 2, 0),
              std::nullopt);
}

TEST(PatternGeneratorCOFF, CanonicalREXWImportsUseTheSameSlotField) {
  for (const uint8_t ModRM : {0x15, 0x25}) {
    std::vector<uint8_t> Code(32, 0x90);
    Code[3] = 0x48;
    Code[4] = 0xff;
    Code[5] = ModRM;
    COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
    Builder.addFunction("caller", 0);
    Builder.setRelocationTarget("__imp_RuntimeAPI");
    Builder.addRelocation(6, COFF::IMAGE_REL_AMD64_REL32);
    PatternGeneratorOptions Opts;
    Opts.EmitReferences = true;
    const auto Out = generate(Builder.build(), Opts);
    ASSERT_EQ(Out.Lines.size(), 1u);
    auto Mod = PatternParser::parseLine(Out.Lines.front());
    ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
    ASSERT_EQ(Mod->References.size(), 1u);
    EXPECT_EQ(Mod->References.front().Name, "__imp_RuntimeAPI");
    EXPECT_EQ(Mod->References.front().Offset, 6u);

    // Address/segment, mandatory and additional REX prefixes stay outside
    // this canonical form. A truncated displacement is never an import.
    for (uint8_t Prefix : {0x64, 0x65, 0x66, 0x67, 0xf2, 0xf3, 0x40, 0x48}) {
      const uint8_t Prefixed[] = {Prefix, 0x48, 0xff, ModRM, 0, 0, 0, 0};
      EXPECT_EQ(coffBranchReferenceOffset(COFF::IMAGE_FILE_MACHINE_AMD64,
                                          COFF::IMAGE_REL_AMD64_REL32, Prefixed,
                                          4, 0),
                std::nullopt);
    }
    const uint8_t Bare[] = {0x48, 0xff, ModRM, 0, 0, 0, 0};
    EXPECT_EQ(coffBranchReferenceOffset(COFF::IMAGE_FILE_MACHINE_AMD64,
                                        COFF::IMAGE_REL_AMD64_REL32, Bare, 3,
                                        0),
              3u);
    for (size_t Size = 3; Size < std::size(Bare); ++Size)
      EXPECT_EQ(coffBranchReferenceOffset(
                    COFF::IMAGE_FILE_MACHINE_AMD64, COFF::IMAGE_REL_AMD64_REL32,
                    ArrayRef(Bare).take_front(Size), 3, 0),
                std::nullopt);
  }
}

TEST(PatternGeneratorCOFF, PrimaryVTableAddressesRetainExactTypeReferences) {
  for (const uint8_t Rex : {0x48, 0x4c}) {
    std::vector<uint8_t> Code(32, 0x90);
    Code[4] = Rex;
    Code[5] = 0x8d;
    Code[6] = 0x05;
    std::fill(Code.begin() + 7, Code.begin() + 11, 0);
    COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
    Builder.setDataTarget();
    Builder.addFunction("constructor", 0);
    Builder.addRelocation(7, COFF::IMAGE_REL_AMD64_REL32);
    Builder.setRelocationTarget("??_7Widget@@6B@");
    PatternGeneratorOptions Opts;
    Opts.EmitReferences = true;
    Generated Out = generate(Builder.build(), Opts);
    ASSERT_EQ(Out.Lines.size(), 1u);
    auto Mod = PatternParser::parseLine(Out.Lines.front());
    ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
    ASSERT_EQ(Mod->References.size(), 1u);
    EXPECT_EQ(Mod->References.front().Name, "??_7Widget@@6B@");
    EXPECT_EQ(Mod->References.front().Offset, 7u);
    EXPECT_TRUE(SignatureMatcher::isFullyVerified(*Mod));
    for (const char *Name : {"global", "??_7Widget@@6BBase@@@", "??_7@@6B@"}) {
      Builder.setRelocationTarget(Name);
      Out = generate(Builder.build(), Opts);
      Mod = PatternParser::parseLine(Out.Lines.front());
      ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
      EXPECT_TRUE(Mod->References.empty()) << Name;
    }
  }
}

TEST(PatternGeneratorCOFF, ThrowInfoReferencesRequireCanonicalExternalData) {
  for (bool EmitData : {false, true}) {
    for (unsigned Variant = 0; Variant < 5; ++Variant) {
      std::vector<uint8_t> Code(32, 0x90);
      Code[4] = 0x48;
      Code[5] = 0x8d;
      Code[6] = 0x15;
      std::fill(Code.begin() + 7, Code.begin() + 11, 0);
      if (Variant == 4)
        Code[7] = 1; // A relocation addend does not name the complete record.
      COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
      if (Variant != 1)
        Builder.setDataTarget();
      if (Variant == 2)
        Builder.setTargetStorage(COFF::IMAGE_SYM_CLASS_STATIC);
      if (Variant == 3)
        Builder.setTargetStorage(COFF::IMAGE_SYM_CLASS_EXTERNAL, 1);
      Builder.addFunction("thrower", 0);
      Builder.addRelocation(7, COFF::IMAGE_REL_AMD64_REL32);
      Builder.setRelocationTarget("_TI2?AVWidget@@");
      PatternGeneratorOptions Opts;
      Opts.EmitReferences = true;
      Opts.EmitDataReferences = EmitData;
      Opts.RequireReferences = true;
      Generated Out = generate(Builder.build(), Opts);
      ASSERT_EQ(Out.Lines.size(), 1u);
      auto Mod = PatternParser::parseLine(Out.Lines.front());
      ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
      ASSERT_EQ(Mod->References.size(), Variant == 0 ? 1u : 0u);
      if (Variant == 0) {
        EXPECT_EQ(Mod->References.front().Name, "_TI2?AVWidget@@");
        EXPECT_EQ(Mod->References.front().Offset, 7u);
        EXPECT_FALSE(Mod->References.front().IsData);
      }
    }
  }
}

TEST(PatternGeneratorCOFF, VTableReferencesRequireCanonicalZeroAddendFields) {
  for (unsigned Variant = 0; Variant != 8; ++Variant) {
    std::vector<uint8_t> Code(32, 0x90);
    Code[4] = 0x48;
    Code[5] = 0x8d;
    Code[6] = 0x05;
    std::fill(Code.begin() + 7, Code.begin() + 11, 0);
    uint16_t Machine = COFF::IMAGE_FILE_MACHINE_AMD64;
    uint16_t Relocation = COFF::IMAGE_REL_AMD64_REL32;
    switch (Variant) {
    case 0:
      Code[3] = 0x67;
      break;
    case 1:
      Code[3] = 0x65;
      break;
    case 2:
      Code[4] = 0x40;
      break;
    case 3:
      Code[5] = 0x8b;
      break;
    case 4:
      Code[6] = 0x04;
      break;
    case 5:
      Code[7] = 8;
      break;
    case 6:
      Relocation = COFF::IMAGE_REL_AMD64_REL32_1;
      break;
    case 7:
      Machine = COFF::IMAGE_FILE_MACHINE_I386;
      Relocation = COFF::IMAGE_REL_I386_REL32;
      break;
    }
    COFFObjectBuilder Builder(Machine, Code);
    Builder.addFunction("constructor", 0);
    Builder.addRelocation(7, Relocation);
    Builder.setRelocationTarget("??_7Widget@@6B@");
    PatternGeneratorOptions Opts;
    Opts.EmitReferences = true;
    Generated Out = generate(Builder.build(), Opts);
    ASSERT_EQ(Out.Lines.size(), 1u);
    auto Mod = PatternParser::parseLine(Out.Lines.front());
    ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
    EXPECT_TRUE(Mod->References.empty()) << Variant;
  }
}

TEST(PatternGeneratorCOFF, IdenticalCodeAtDifferentAddressesIsNotAliased) {
  const std::vector<uint8_t> Body = sequentialCode(32);
  std::vector<uint8_t> Code = Body;
  Code.insert(Code.end(), Body.begin(), Body.end());
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
  Builder.addFunction("first", 0);
  Builder.addFunction("second", 32);
  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 2u);
  for (size_t I = 0; I < Out.Lines.size(); ++I) {
    auto Mod = PatternParser::parseLine(Out.Lines[I]);
    ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
    ASSERT_EQ(Mod->PublicNames.size(), 1u);
    EXPECT_EQ(Mod->PublicNames.front().Name, I == 0 ? "first" : "second");
  }
}

TEST(PatternGeneratorCOFF, ReferencesToAnyOwnAliasAreRecursion) {
  std::vector<uint8_t> Code = sequentialCode(48);
  Code[8] = 0xE8;
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
  Builder.addFunction("caller", 0);
  Builder.addFunction("target", 0);
  Builder.addRelocation(9, COFF::IMAGE_REL_AMD64_REL32);
  PatternGeneratorOptions Opts;
  Opts.EmitReferences = true;
  Opts.TailLen = 0xFFFF;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  auto Mod = PatternParser::parseLine(Out.Lines.front());
  ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
  EXPECT_EQ(Mod->PublicNames.size(), 2u);
  EXPECT_TRUE(Mod->References.empty());
  EXPECT_TRUE(SignatureMatcher::isFullyVerified(*Mod));
}

TEST(PatternGeneratorCOFF, CollisionEvidenceRetainsOwnAddressAndBranchAliases) {
  for (const std::vector<uint8_t> &Encoding :
       {std::vector<uint8_t>{0xe8}, std::vector<uint8_t>{0xe9},
        std::vector<uint8_t>{0x0f, 0x85},
        std::vector<uint8_t>{0x48, 0x8d, 0x05}}) {
    std::vector<uint8_t> Code = sequentialCode(48);
    std::copy(Encoding.begin(), Encoding.end(), Code.begin());
    std::fill_n(Code.begin() + Encoding.size(), sizeof(uint32_t), 0);
    COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
    Builder.addFunction("self", 0);
    Builder.addFunction("self_alias", 0);
    Builder.setRelocationTarget("self_alias");
    Builder.addRelocation(Encoding.size(), COFF::IMAGE_REL_AMD64_REL32);
    PatternGeneratorOptions Opts;
    Opts.EmitReferences = true;
    Opts.TailLen = 0xffff;
    Opts.AlternateNames["self_alias"] = {"self"};
    for (bool Caller : {false, true}) {
      Opts.RequireCaller = Caller;
      const auto Ordinary = generate(Builder.build(), Opts);
      ASSERT_EQ(Ordinary.Lines.size(), 1u);
      EXPECT_EQ(Ordinary.Lines.front().find('^'), std::string::npos);
    }
    Opts.RequireCaller = false;
    Opts.CollisionEvidence = true;
    const auto Evidence = generate(Builder.build(), Opts);
    ASSERT_EQ(Evidence.Lines.size(), 1u);
    const std::string Site = "^000" + std::to_string(Encoding.size()) + " ";
    EXPECT_NE(Evidence.Lines.front().find(Site + "self "), std::string::npos);
    EXPECT_NE((Evidence.Lines.front() + " ").find(Site + "self_alias "),
              std::string::npos);
    auto Rejected = PatternParser::parseLine(Evidence.Lines.front());
    ASSERT_FALSE(bool(Rejected));
    consumeError(Rejected.takeError());
  }
}

TEST(PatternGeneratorCOFF, AlternateNamesComeFromTheDirectives) {
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_I386, sequentialCode(32));
  Builder.addFunction("_f", 0);
  // MSVC's own spelling, a quoted one, another option, and a second
  // alternate for one symbol.
  Builder.setDirectives(
      "\xEF\xBB\xBF   /ALTERNATENAME:___filter=___filter_default "
      "/DEFAULTLIB:\"LIBCMT\" "
      "\"-alternatename:_pRawDllMain=_pDefaultRawDllMain\" "
      "/alternatename:___filter=___filter_other ");
  const std::vector<uint8_t> Bytes = Builder.build();
  auto ObjOrErr = object::ObjectFile::createObjectFile(MemoryBufferRef(
      StringRef(reinterpret_cast<const char *>(Bytes.data()), Bytes.size()),
      "directives.obj"));
  ASSERT_TRUE(static_cast<bool>(ObjOrErr)) << toString(ObjOrErr.takeError());

  std::map<std::string, std::vector<std::string>, std::less<>> Names;
  collectAlternateNames(**ObjOrErr, Names);
  ASSERT_EQ(Names.size(), 2u);
  EXPECT_EQ(Names["___filter"],
            (std::vector<std::string>{"___filter_default", "___filter_other"}));
  EXPECT_EQ(Names["_pRawDllMain"],
            std::vector<std::string>{"_pDefaultRawDllMain"});
}

TEST(PatternGeneratorCOFF, ABranchToASymbolWithAnAlternateNamesBoth) {
  // x86: a call at 8 to `target`, which a link without it resolves to
  // `target_default`.
  std::vector<uint8_t> Code = sequentialCode(48);
  Code[8] = 0xE8;
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_I386, Code);
  Builder.addFunction("caller", 0);
  Builder.addRelocation(9, COFF::IMAGE_REL_I386_REL32);
  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Opts.EmitReferences = true;
  Opts.AlternateNames["target"] = {"target_default", "caller"};

  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  auto Mod = PatternParser::parseLine(Out.Lines[0]);
  ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
  // The routine's own name is no alternate the image confirms.
  ASSERT_EQ(Mod->References.size(), 2u) << Out.Lines[0];
  EXPECT_EQ(Mod->References[0].Offset, 9u);
  EXPECT_EQ(Mod->References[0].Name, "target");
  EXPECT_EQ(Mod->References[1].Offset, 9u);
  EXPECT_EQ(Mod->References[1].Name, "target_default");
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
  std::vector<uint8_t> Code = {0x55, 0x48, 0x89, 0xE5, 0xE8, 0, 0, 0,
                               0,    0x48, 0x8B, 0x2D, 0,    0, 0, 0};
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
  EXPECT_TRUE(
      StringRef(Out.Lines[0]).starts_with("554889E5E8........488B2D........"));
  std::vector<uint8_t> Linked = linked(
      linked(Code, 5, {0x16, 0xFE, 0xFF, 0xFF}), 12, {0x5D, 0x3C, 0x1A, 0});
  EXPECT_TRUE(lineMatches(Out.Lines[0], Linked));
}

TEST(PatternGeneratorELF, RelaxedGotAccessesKeepTheirMatch) {
  // mov rax,[rip+foo@GOTPCREL]; mov eax,[rip+bar@GOTPCREL];
  // call [rip+baz@GOTPCREL]; then code no relocation touches.
  std::vector<uint8_t> Code = {0x48, 0x8B, 0x05, 0, 0, 0, 0,
                               0x8B, 0x05, 0,    0, 0, 0, 0xFF,
                               0x15, 0,    0,    0, 0};
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
  std::vector<uint8_t> Relaxed =
      linked(Code, 0,
             {0x48, 0x8D, 0x05, 0x10, 0x20, 0, 0, 0xC7, 0xC0, 0x30, 0x40, 0x50,
              0, 0x67, 0xE8, 0x60, 0x70, 0, 0});
  EXPECT_TRUE(lineMatches(Out.Lines[0], Relaxed));
}

TEST(PatternGeneratorELF, TlsSequencesAreWildcardedWhole) {
  // General dynamic: data16 lea rdi,[rip+x@tlsgd]; data16 data16 rex64 call
  // __tls_get_addr@PLT.
  std::vector<uint8_t> Code = {0x66, 0x48, 0x8D, 0x3D, 0, 0, 0, 0,
                               0x66, 0x66, 0x48, 0xE8, 0, 0, 0, 0};
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
  EXPECT_TRUE(lineMatches(
      Out.Lines[0], linked(Code, 0, {0x8D, 0x83, 0x10, 0xE0, 0xFF, 0xFF})));
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
  EXPECT_TRUE(
      StringRef(Out.Lines[0])
          .ends_with(":0000 puts :0000 _IO_puts :0000 __IO_puts_internal"))
      << Out.Lines[0];
  EXPECT_TRUE(StringRef(Out.Lines[1]).ends_with(":0000 fputs"));
  EXPECT_EQ(Out.Stats.Functions, 2u);
  auto Parsed = PatternParser::parseLine(Out.Lines[0]);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  EXPECT_EQ(Parsed->PublicNames.size(), 3u);
}

/// x86-64 code that calls a routine through the PLT at 4 and loads a
/// RIP-relative word at 9 -- a PC32 field that is no branch -- then runs to
/// 32 bytes.
std::vector<uint8_t> callingCode() {
  std::vector<uint8_t> Code = {0x55, 0x48, 0x89, 0xE5, 0xE8, 0, 0, 0,
                               0,    0x48, 0x8B, 0x05, 0,    0, 0, 0};
  std::vector<uint8_t> Rest = sequentialCode(16);
  Code.insert(Code.end(), Rest.begin(), Rest.end());
  return Code;
}

TEST(PatternGeneratorELF, DirectBranchesBecomeReferencesOnRequest) {
  // ctype_byname<wchar_t>::do_toupper and do_tolower: the same bytes, told
  // apart only by the routine each calls.
  std::vector<uint8_t> Code = callingCode();
  const std::vector<uint8_t> Twin = callingCode();
  Code.insert(Code.end(), Twin.begin(), Twin.end());
  ELFObjectBuilder Builder(ELF::EM_X86_64, true, Code);
  Builder.addFunction("upper", 0, 32);
  Builder.addFunction("lower", 32, 32);
  Builder.addRelocation(5, ELF::R_X86_64_PLT32, "towupper_l");
  Builder.addRelocation(12, ELF::R_X86_64_PC32, "table");
  Builder.addRelocation(37, ELF::R_X86_64_PLT32, "towlower_l");
  Builder.addRelocation(44, ELF::R_X86_64_PC32, "table");
  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;

  Generated Plain = generate(Builder.build(), Opts);
  ASSERT_EQ(Plain.Lines.size(), 2u);
  EXPECT_EQ(Plain.Lines[0].find('^'), std::string::npos)
      << "references are off unless asked for";

  Opts.EmitReferences = true;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 2u);
  const std::pair<const char *, const char *> Expected[] = {
      {"upper", "towupper_l"}, {"lower", "towlower_l"}};
  for (size_t I = 0; I < 2; ++I) {
    auto Mod = PatternParser::parseLine(Out.Lines[I]);
    ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
    ASSERT_EQ(Mod->PublicNames.size(), 1u);
    EXPECT_EQ(Mod->PublicNames[0].Name, Expected[I].first);
    ASSERT_EQ(Mod->References.size(), 1u) << Out.Lines[I];
    EXPECT_EQ(Mod->References[0].Offset, 5u);
    EXPECT_EQ(Mod->References[0].Name, Expected[I].second);
  }
}

TEST(PatternGeneratorELF, ReferencesNameOnlyOtherRoutines) {
  // Calls to the routine itself under an alias, through the section symbol,
  // and to another routine.
  std::vector<uint8_t> Code = {0x55, 0xE8, 0, 0,    0, 0, 0xE8, 0,
                               0,    0,    0, 0xE8, 0, 0, 0,    0};
  std::vector<uint8_t> Rest = sequentialCode(16);
  Code.insert(Code.end(), Rest.begin(), Rest.end());
  ELFObjectBuilder Builder(ELF::EM_X86_64, true, Code);
  Builder.addFunction("recurse", 0);
  Builder.addFunction("__recurse", 0);
  Builder.addRelocation(2, ELF::R_X86_64_PLT32, "__recurse");
  Builder.addRelocation(7, ELF::R_X86_64_PC32, ELFObjectBuilder::SectionSymbol);
  Builder.addRelocation(12, ELF::R_X86_64_PLT32, "other");
  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Opts.EmitReferences = true;

  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  auto Mod = PatternParser::parseLine(Out.Lines[0]);
  ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
  ASSERT_EQ(Mod->References.size(), 1u) << Out.Lines[0];
  EXPECT_EQ(Mod->References[0].Offset, 12u);
  EXPECT_EQ(Mod->References[0].Name, "other");
}

TEST(PatternGeneratorELF, CollisionEvidenceRetainsSelfReferenceAliases) {
  std::vector<uint8_t> Code = sequentialCode(48);
  Code[0] = 0xe8;
  ELFObjectBuilder Builder(ELF::EM_X86_64, true, Code);
  Builder.addFunction("self", 0);
  Builder.addFunction("self_alias", 0);
  Builder.addRelocation(1, ELF::R_X86_64_PLT32, "self_alias");
  PatternGeneratorOptions Opts;
  Opts.EmitReferences = true;
  const auto Ordinary = generate(Builder.build(), Opts);
  ASSERT_EQ(Ordinary.Lines.size(), 1u);
  EXPECT_EQ(Ordinary.Lines.front().find('^'), std::string::npos);
  Opts.CollisionEvidence = true;
  const auto Evidence = generate(Builder.build(), Opts);
  ASSERT_EQ(Evidence.Lines.size(), 1u);
  EXPECT_NE((Evidence.Lines.front() + " ").find("^0001 self_alias "),
            std::string::npos);
}

TEST(PatternGeneratorELF, ACallARelaxationMayEraseIsNoReference) {
  // General dynamic TLS: data16 lea rdi,[rip+x@tlsgd]; data16 data16 rex64
  // call __tls_get_addr@PLT, which a static link rewrites -- then a call
  // that stays.
  std::vector<uint8_t> Code = {0x66, 0x48, 0x8D, 0x3D, 0,    0, 0,
                               0,    0x66, 0x66, 0x48, 0xE8, 0, 0,
                               0,    0,    0xE8, 0,    0,    0, 0};
  std::vector<uint8_t> Rest = sequentialCode(24);
  Code.insert(Code.end(), Rest.begin(), Rest.end());
  ELFObjectBuilder Builder(ELF::EM_X86_64, true, Code);
  Builder.addFunction("tls_reader", 0);
  Builder.addRelocation(4, ELF::R_X86_64_TLSGD, "x");
  Builder.addRelocation(12, ELF::R_X86_64_PLT32, "__tls_get_addr");
  Builder.addRelocation(17, ELF::R_X86_64_PLT32, "kept");
  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Opts.EmitReferences = true;

  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  auto Mod = PatternParser::parseLine(Out.Lines[0]);
  ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
  ASSERT_EQ(Mod->References.size(), 1u) << Out.Lines[0];
  EXPECT_EQ(Mod->References[0].Offset, 17u);
  EXPECT_EQ(Mod->References[0].Name, "kept");
}

TEST(PatternGeneratorELF, ThumbAndAArch64BranchesAreReferences) {
  // A Thumb-2 BL at 4 and B.W at 8; an AArch64 BL at 4 and B at 8.
  std::vector<uint8_t> Code = sequentialCode(40);
  ELFObjectBuilder Thumb(ELF::EM_ARM, false, Code);
  Thumb.addFunction("thumb", 0);
  Thumb.addRelocation(4, ELF::R_ARM_THM_CALL, "callee");
  Thumb.addRelocation(8, ELF::R_ARM_THM_JUMP24, "tail");
  ELFObjectBuilder AArch64(ELF::EM_AARCH64, true, Code);
  AArch64.addFunction("a64", 0);
  AArch64.addRelocation(4, ELF::R_AARCH64_CALL26, "callee");
  AArch64.addRelocation(8, ELF::R_AARCH64_JUMP26, "tail");
  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Opts.EmitReferences = true;

  for (ELFObjectBuilder *Builder : {&Thumb, &AArch64}) {
    Generated Out = generate(Builder->build(), Opts);
    ASSERT_EQ(Out.Lines.size(), 1u);
    auto Mod = PatternParser::parseLine(Out.Lines[0]);
    ASSERT_TRUE(static_cast<bool>(Mod)) << toString(Mod.takeError());
    ASSERT_EQ(Mod->References.size(), 2u) << Out.Lines[0];
    EXPECT_EQ(Mod->References[0].Offset, 4u);
    EXPECT_EQ(Mod->References[0].Name, "callee");
    EXPECT_EQ(Mod->References[1].Offset, 8u);
    EXPECT_EQ(Mod->References[1].Name, "tail");
  }
}

TEST(PatternGeneratorELF, BranchReferencesUseEachMachinesOffset) {
  const std::vector<uint8_t> Call = {0x55, 0xE8, 0, 0, 0, 0, 0xC3};
  EXPECT_EQ(
      elfBranchReferenceOffset(ELF::EM_X86_64, ELF::R_X86_64_PLT32, Call, 2),
      2u);
  EXPECT_EQ(elfBranchReferenceOffset(ELF::EM_386, ELF::R_386_PC32, Call, 2),
            2u);
  // The same field after a non-branch opcode, at the function's first byte,
  // or of a type no call has, is no branch.
  EXPECT_EQ(
      elfBranchReferenceOffset(ELF::EM_X86_64, ELF::R_X86_64_PC32, Call, 3),
      std::nullopt);
  EXPECT_EQ(elfBranchReferenceOffset(ELF::EM_386, ELF::R_386_PLT32,
                                     ArrayRef<uint8_t>(Call).drop_front(2), 0),
            std::nullopt);
  EXPECT_EQ(elfBranchReferenceOffset(ELF::EM_X86_64, ELF::R_X86_64_GOTPCRELX,
                                     Call, 2),
            std::nullopt);
  const std::vector<uint8_t> Words(16, 0);
  EXPECT_EQ(elfBranchReferenceOffset(ELF::EM_AARCH64, ELF::R_AARCH64_CALL26,
                                     Words, 8),
            8u);
  EXPECT_EQ(elfBranchReferenceOffset(ELF::EM_AARCH64,
                                     ELF::R_AARCH64_ADR_PREL_PG_HI21, Words, 8),
            std::nullopt);
  // A branch that runs past the function's end is not the function's.
  EXPECT_EQ(elfBranchReferenceOffset(ELF::EM_AARCH64, ELF::R_AARCH64_JUMP26,
                                     Words, 14),
            std::nullopt);
  EXPECT_EQ(
      elfBranchReferenceOffset(ELF::EM_ARM, ELF::R_ARM_THM_CALL, Words, 6), 6u);
  EXPECT_EQ(
      elfBranchReferenceOffset(ELF::EM_ARM, ELF::R_ARM_THM_JUMP19, Words, 6),
      std::nullopt);
  // An ARM-state branch is stated one byte past its instruction.
  EXPECT_EQ(elfBranchReferenceOffset(ELF::EM_ARM, ELF::R_ARM_CALL, Words, 8),
            9u);
  EXPECT_EQ(elfBranchReferenceOffset(ELF::EM_ARM, ELF::R_ARM_JUMP24, Words, 4),
            5u);
  EXPECT_EQ(elfBranchReferenceOffset(ELF::EM_ARM, ELF::R_ARM_PLT32, Words, 0),
            1u);
  // Neither kind of branch is where its instruction set puts none.
  EXPECT_EQ(elfBranchReferenceOffset(ELF::EM_ARM, ELF::R_ARM_CALL, Words, 6),
            std::nullopt);
  EXPECT_EQ(
      elfBranchReferenceOffset(ELF::EM_ARM, ELF::R_ARM_THM_CALL, Words, 5),
      std::nullopt);
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

TEST(PatternGeneratorELF, AFunctionEndsAtItsSymbolsSize) {
  // Twenty bytes of code, twelve of NOP padding, then the next function.
  std::vector<uint8_t> Code = sequentialCode(20);
  Code.insert(Code.end(), 12, 0x90);
  std::vector<uint8_t> Next = sequentialCode(32);
  Code.insert(Code.end(), Next.begin(), Next.end());
  ELFObjectBuilder Sized(ELF::EM_X86_64, true, Code);
  Sized.addFunction("padded", 0, 20);
  Sized.addFunction("next", 32, 32);
  Generated Out = generate(Sized.build());
  ASSERT_EQ(Out.Lines.size(), 2u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).ends_with(" 0014 :0000 padded"))
      << Out.Lines[0];

  // A symbol with no size runs to the next symbol, as before.
  ELFObjectBuilder Unsized(ELF::EM_X86_64, true, Code);
  Unsized.addFunction("padded", 0);
  Unsized.addFunction("next", 32);
  Out = generate(Unsized.build());
  ASSERT_EQ(Out.Lines.size(), 2u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).ends_with(" 0020 :0000 padded"))
      << Out.Lines[0];
}

TEST(PatternGeneratorCOFF,
     ContextualFunctionsRequireTheirWholeBodyAndReferences) {
  std::vector<uint8_t> Code{0x48, 0x83, 0xec, 0x28, 0xff, 0x15, 0,   0,
                            0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
  Builder.addFunction("wrapper", 0);
  Builder.addRelocation(6, COFF::IMAGE_REL_AMD64_REL32);
  Builder.setRelocationTarget("__imp_RuntimeAPI");
  PatternGeneratorOptions Opts;
  Opts.EmitReferences = true;
  EXPECT_TRUE(generate(Builder.build(), Opts).Lines.empty());
  Opts.RequireReferences = true;
  Opts.LeadingLen = 4;
  Opts.TailLen = 0; // Contextual output still covers the entire routine.
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  auto Mod = PatternParser::parseLine(Out.Lines.front());
  ASSERT_TRUE(bool(Mod)) << toString(Mod.takeError());
  EXPECT_TRUE(Mod->RequireReferences);
  EXPECT_TRUE(SignatureMatcher::isFullyVerified(*Mod));
  EXPECT_LT(SignatureMatcher::fixedByteCount(*Mod),
            SignatureMatcher::MinStatedBytes);
  Opts.EmitReferences = false;
  EXPECT_TRUE(generate(Builder.build(), Opts).Lines.empty());
}

TEST(PatternGeneratorCOFF, FunctionAddressReferencesRequireFunctionSymbols) {
  const std::vector<uint8_t> Code{0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3};
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
  Builder.addFunction("address_getter", 0);
  Builder.setRelocationTarget("callee");
  Builder.addRelocation(3, COFF::IMAGE_REL_AMD64_REL32);
  PatternGeneratorOptions Opts;
  Opts.EmitReferences = Opts.RequireReferences = true;
  auto Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  auto Mod = PatternParser::parseLine(Out.Lines.front());
  ASSERT_TRUE(bool(Mod)) << toString(Mod.takeError());
  EXPECT_TRUE(Mod->RequireReferences);
  ASSERT_EQ(Mod->References.size(), 1u);
  EXPECT_EQ(Mod->References.front().Name, "callee");
  EXPECT_EQ(Mod->References.front().Offset, 3u);
  Builder.setDataTarget();
  EXPECT_TRUE(generate(Builder.build(), Opts).Lines.empty());
}

TEST(PatternGeneratorCOFF, FunctionAddressesAfterPrefixLookingOperands) {
  // F0 is the previous LEA's SIB byte, not a LOCK prefix on the next LEA.
  COFFObjectBuilder Builder(
      COFF::IMAGE_FILE_MACHINE_AMD64,
      {0x48, 0x8d, 0x0c, 0xf0, 0x4c, 0x8d, 0x0d, 0, 0, 0, 0, 0xc3});
  Builder.addFunction("registration", 0);
  Builder.setRelocationTarget("callback");
  Builder.addRelocation(7, COFF::IMAGE_REL_AMD64_REL32);
  PatternGeneratorOptions Opts;
  Opts.EmitReferences = Opts.RequireReferences = true;
  const auto Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  auto Mod = PatternParser::parseLine(Out.Lines.front());
  ASSERT_TRUE(bool(Mod)) << toString(Mod.takeError());
  ASSERT_EQ(Mod->References.size(), 1u);
  EXPECT_EQ(Mod->References.front().Name, "callback");
  EXPECT_EQ(Mod->References.front().Offset, 7u);
}

TEST(PatternGeneratorCOFF, CollisionEvidenceIncludesWeakUnreferencedFunctions) {
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, {0x33, 0xc0, 0xc3});
  Builder.addFunction("zero", 0);
  PatternGeneratorOptions Opts;
  EXPECT_TRUE(generate(Builder.build(), Opts).Lines.empty());
  Opts.CollisionEvidence = true;
  Opts.LeadingLen = 1;
  Generated Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_NE(Out.Lines.front().find(" !evidence "), std::string::npos);
  auto Mod = PatternParser::parseLine(Out.Lines.front());
  ASSERT_FALSE(bool(Mod));
  EXPECT_NE(toString(Mod.takeError()).find("not a matchable signature"),
            std::string::npos);
}

TEST(PatternGeneratorCOFF,
     CallerContextIncludesCompleteBodiesAndTheirConstraints) {
  COFFObjectBuilder Leaf(COFF::IMAGE_FILE_MACHINE_AMD64, {0x33, 0xc0, 0xc3});
  Leaf.addFunction("zero", 0);
  PatternGeneratorOptions Opts;
  Opts.EmitReferences = Opts.RequireCaller = true;
  Opts.LeadingLen = 1;
  const auto Out = generate(Leaf.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  auto Mod = PatternParser::parseLine(Out.Lines.front());
  ASSERT_TRUE(bool(Mod)) << toString(Mod.takeError());
  EXPECT_TRUE(Mod->RequireCaller);
  EXPECT_TRUE(SignatureMatcher::isFullyVerified(*Mod));
  EXPECT_EQ(Mod->TotalLen, 3u);
  COFFObjectBuilder Caller(COFF::IMAGE_FILE_MACHINE_AMD64,
                           {0xe8, 0, 0, 0, 0, 0xc3});
  Caller.addFunction("wrapper", 0);
  Caller.addRelocation(1, COFF::IMAGE_REL_AMD64_REL32);
  Caller.setRelocationTarget("zero");
  const auto Calls = generate(Caller.build(), Opts);
  ASSERT_EQ(Calls.Lines.size(), 1u);
  auto Child = PatternParser::parseLine(Calls.Lines.front());
  ASSERT_TRUE(bool(Child)) << toString(Child.takeError());
  EXPECT_TRUE(Child->RequireCaller);
  ASSERT_EQ(Child->References.size(), 1u);
  EXPECT_EQ(Child->References.front().Name, "zero");
}

TEST(PatternGeneratorCOFF,
     ExternalDataRequiresTypedCanonicalZeroAddendRelocations) {
  struct Form {
    std::vector<uint8_t> Bytes;
    unsigned Field;
    uint16_t Relocation;
  };
  const Form Forms[] = {
      {{0xff, 0x15, 0, 0, 0, 0}, 2, COFF::IMAGE_REL_AMD64_REL32},
      {{0xff, 0x25, 0, 0, 0, 0}, 2, COFF::IMAGE_REL_AMD64_REL32},
      {{0x48, 0xff, 0x25, 0, 0, 0, 0}, 3, COFF::IMAGE_REL_AMD64_REL32},
      {{0x48, 0x8b, 0x05, 0, 0, 0, 0}, 3, COFF::IMAGE_REL_AMD64_REL32},
      {{0x48, 0x8d, 0x0d, 0, 0, 0, 0}, 3, COFF::IMAGE_REL_AMD64_REL32},
      {{0x8b, 0x05, 0, 0, 0, 0}, 2, COFF::IMAGE_REL_AMD64_REL32},
      {{0x83, 0x3d, 0, 0, 0, 0, 0}, 2, COFF::IMAGE_REL_AMD64_REL32_1},
      {{0xc7, 0x05, 0, 0, 0, 0, 1, 0, 0, 0}, 2, COFF::IMAGE_REL_AMD64_REL32_4},
  };
  for (const auto &Form : Forms) {
    for (unsigned Case = 0; Case < 9; ++Case) {
      std::vector<uint8_t> Code(16, 0x90);
      Code.insert(Code.end(), Form.Bytes.begin(), Form.Bytes.end());
      Code.push_back(0xc3);
      if (Case == 1)
        Code[16 + Form.Field] = 1;
      if (Case == 2)
        Code[15] = 0x67;
      COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
      Builder.addFunction("root", 0);
      Builder.setRelocationTarget("external_global");
      if (Case != 3)
        Builder.setDataTarget();
      if (Case == 4)
        Builder.setTargetStorage(COFF::IMAGE_SYM_CLASS_STATIC);
      if (Case == 5)
        Builder.setTargetStorage(COFF::IMAGE_SYM_CLASS_EXTERNAL,
                                 COFF::IMAGE_SYM_ABSOLUTE);
      if (Case == 6)
        Builder.setTargetStorage(COFF::IMAGE_SYM_CLASS_EXTERNAL,
                                 1); // Executable definition.
      Builder.addRelocation(16 + Form.Field, Case == 7
                                                 ? COFF::IMAGE_REL_AMD64_REL32_2
                                                 : Form.Relocation);
      PatternGeneratorOptions Opts;
      Opts.EmitReferences = true;
      Opts.EmitDataReferences = true;
      Opts.RequireCaller = Case != 8;
      Opts.TailLen = 0xffff;
      const Generated Out = generate(Builder.build(), Opts);
      ASSERT_EQ(Out.Lines.size(), 1u);
      auto Mod = PatternParser::parseLine(Out.Lines[0]);
      ASSERT_TRUE(bool(Mod)) << llvm::toString(Mod.takeError());
      const auto Count = llvm::count_if(
          Mod->References, [](const FuncRef &Ref) { return Ref.IsData; });
      EXPECT_EQ(Count, Case == 0 ? 1 : 0) << "case " << Case;
      if (Case == 0) {
        EXPECT_EQ(Mod->References.front().Offset, 16 + Form.Field);
        EXPECT_EQ(Mod->References.front().Name, "external_global");
      }
    }
  }
}

TEST(PatternGeneratorCOFF, ExternalDataKeepsAlternativesAndEvidenceKinds) {
  std::vector<uint8_t> Code = {0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3};
  COFFObjectBuilder Builder(COFF::IMAGE_FILE_MACHINE_AMD64, Code);
  Builder.addFunction("getter", 0);
  Builder.setDataTarget();
  Builder.addRelocation(3, COFF::IMAGE_REL_AMD64_REL32);
  PatternGeneratorOptions Opts;
  Opts.EmitReferences = Opts.EmitDataReferences = Opts.RequireReferences = true;
  Opts.AlternateNames["target"] = {"alternate"};
  auto Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  auto Mod = PatternParser::parseLine(Out.Lines[0]);
  ASSERT_TRUE(bool(Mod));
  ASSERT_EQ(Mod->References.size(), 2u);
  EXPECT_TRUE(Mod->References[0].IsData);
  EXPECT_TRUE(Mod->References[1].IsData);
  EXPECT_TRUE(Mod->RequireReferences);
  Opts.RequireReferences = false;
  Opts.CollisionEvidence = true;
  Out = generate(Builder.build(), Opts);
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).contains("!evidence"));
  EXPECT_TRUE(StringRef(Out.Lines[0]).contains("&0003 target"));
}
