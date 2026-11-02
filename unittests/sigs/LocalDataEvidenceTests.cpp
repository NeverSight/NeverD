//===- LocalDataEvidenceTests.cpp - Same-object data relations ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source ownership, exact canonical operands and incomplete counterclaims.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternGenerator.h"
#include "neverd/sigs/PatternParser.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace llvm;
using namespace neverd::sigs;

namespace {

struct LocalObject {
  std::string Anchor = "anchor";
  std::string Static = "local";
  uint8_t AnchorClass = COFF::IMAGE_SYM_CLASS_EXTERNAL;
  uint8_t DataClass = COFF::IMAGE_SYM_CLASS_STATIC;
  int16_t DataSection = 2;
  uint32_t DataValue = 0;
  uint32_t AnchorAddend = 0;
  uint32_t GetterAddend = 0;
  bool SeparateTarget = false;
  bool AuxiliaryTarget = false;
  uint32_t GetterValue = 8;
  uint8_t GetterClass = COFF::IMAGE_SYM_CLASS_STATIC;
  int16_t GetterSection = 2;
  uint16_t Relocation = COFF::IMAGE_REL_AMD64_REL32;
  uint32_t DataFlags = COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA |
                       COFF::IMAGE_SCN_MEM_READ | COFF::IMAGE_SCN_MEM_WRITE;
  bool DuplicateAnchor = false;
  bool Overlap = false;
  bool PrecedingOverlap = false;
  bool FunctionTarget = false;
  bool FunctionAnchor = true;

  std::string build() const {
    std::string Bytes;
    auto Put = [&](uint64_t Value, unsigned Width) {
      for (unsigned I = 0; I < Width; ++I)
        Bytes.push_back(Value >> (I * 8));
    };
    const unsigned Relocations = Overlap || PrecedingOverlap ? 3 : 2;
    const unsigned SymbolOffset = 100 + 32 + Relocations * 10;
    Put(COFF::IMAGE_FILE_MACHINE_AMD64, 2);
    Put(2, 2);
    Put(0, 4);
    Put(SymbolOffset, 4);
    Put(3 + DuplicateAnchor + SeparateTarget + AuxiliaryTarget, 4);
    Put(0, 4);
    auto Section = [&](StringRef Name, unsigned Size, unsigned Raw,
                       unsigned Relocs, unsigned Count, uint32_t Flags) {
      Bytes += Name.str();
      Bytes.append(8 - Name.size(), '\0');
      Put(0, 8);
      Put(Size, 4);
      Put(Raw, 4);
      Put(Relocs, 4);
      Put(0, 4);
      Put(Count, 2);
      Put(0, 2);
      Put(Flags, 4);
    };
    Section(".text", 32, 100, 132, Relocations,
            COFF::IMAGE_SCN_CNT_CODE | COFF::IMAGE_SCN_MEM_READ |
                COFF::IMAGE_SCN_MEM_EXECUTE);
    Section(".bss", 16, 0, 0, 0, DataFlags);
    Bytes.append(16, '\x90');
    for (uint32_t Addend : {AnchorAddend, GetterAddend}) {
      Bytes += "\x48\x8b\x05";
      Put(Addend, 4);
      Put(0xc3, 1);
    }
    for (unsigned Offset : {19u, 27u}) {
      Put(Offset, 4);
      Put(Offset == 27 && AuxiliaryTarget  ? 2
          : Offset == 27 && SeparateTarget ? 3 + AuxiliaryTarget
                                           : 2 + AuxiliaryTarget,
          4);
      Put(Relocation, 2);
    }
    if (Overlap || PrecedingOverlap) {
      Put(PrecedingOverlap ? 23 : 18, 4);
      Put(2, 4);
      Put(PrecedingOverlap ? COFF::IMAGE_REL_AMD64_ADDR64
                           : COFF::IMAGE_REL_AMD64_REL32,
          2);
    }
    std::string Strings;
    auto Symbol = [&](StringRef Name, unsigned Value, int16_t Section,
                      bool Function, uint8_t Class, unsigned Aux = 0) {
      if (Name.size() > 8) {
        Put(0, 4);
        Put(4 + Strings.size(), 4);
        Strings += Name.str();
        Strings.push_back('\0');
      } else {
        Bytes += Name.str();
        Bytes.append(8 - Name.size(), '\0');
      }
      Put(Value, 4);
      Put(uint16_t(Section), 2);
      Put(Function
              ? COFF::IMAGE_SYM_DTYPE_FUNCTION << COFF::SCT_COMPLEX_TYPE_SHIFT
              : 0,
          2);
      Put(Class, 1);
      Put(Aux, 1);
    };
    Symbol(Anchor, 0, 1, FunctionAnchor, AnchorClass);
    Symbol("getter", 24, 1, true, COFF::IMAGE_SYM_CLASS_EXTERNAL,
           AuxiliaryTarget);
    if (AuxiliaryTarget)
      Symbol("not_data", 8, 2, false, COFF::IMAGE_SYM_CLASS_STATIC);
    Symbol(Static, DataValue, DataSection, FunctionTarget, DataClass);
    if (SeparateTarget)
      Symbol("target", GetterValue, GetterSection, false, GetterClass);
    if (DuplicateAnchor)
      Symbol(Anchor, 24, 1, true, AnchorClass);
    Put(4 + Strings.size(), 4);
    Bytes += Strings;
    return Bytes;
  }
};

std::vector<PatternModule> generateLocal(const LocalObject &Input,
                                         bool Enable = true,
                                         bool Layout = false) {
  const std::string Bytes = Input.build();
  auto Object =
      object::ObjectFile::createObjectFile(MemoryBufferRef(Bytes, "local.obj"));
  if (!Object) {
    ADD_FAILURE() << toString(Object.takeError());
    return {};
  }
  PatternGeneratorOptions Options;
  Options.EmitReferences = Options.EmitDataReferences = Options.RequireCaller =
      true;
  if (Enable)
    Options.LocalDataAnchors = {Input.Anchor};
  Options.EmitLocalDataLayout = Layout;
  std::string Text;
  raw_string_ostream Output(Text);
  generatePatterns(**Object, Options, Output);
  auto Parsed = PatternParser::parseText(Text);
  if (!Parsed) {
    ADD_FAILURE() << toString(Parsed.takeError());
    return {};
  }
  return std::move(*Parsed);
}

} // namespace

TEST(LocalDataEvidence, ARelationNamesTheAnchorOperandNotAStaticSpelling) {
  LocalObject Source;
  for (const auto *Name : {"local", "another_static_spelling"}) {
    Source.Static = Name;
    const auto Modules = generateLocal(Source);
    ASSERT_EQ(Modules.size(), 2u);
    for (const auto &Module : Modules) {
      ASSERT_EQ(Module.References.size(), 1u);
      EXPECT_TRUE(Module.References[0].IsData);
      EXPECT_TRUE(Module.References[0].IsLocalData);
      EXPECT_EQ(Module.References[0].Name, "00000013:anchor");
    }
  }
  Source.Anchor = "different_external_anchor";
  const auto Modules = generateLocal(Source);
  ASSERT_EQ(Modules.size(), 2u);
  ASSERT_EQ(Modules[1].References.size(), 1u);
  EXPECT_EQ(Modules[1].References[0].Name,
            "00000013:different_external_anchor");
}

TEST(LocalDataEvidence, MissingAmbiguousOrUnsupportedOwnersLeaveRawBodies) {
  for (unsigned Case = 0; Case < 17; ++Case) {
    LocalObject Source;
    switch (Case) {
    case 0:
      break; // No opt-in.
    case 1:
      Source.AnchorClass = COFF::IMAGE_SYM_CLASS_WEAK_EXTERNAL;
      break;
    case 2:
      Source.DuplicateAnchor = true;
      break;
    case 3:
      Source.DataSection = COFF::IMAGE_SYM_UNDEFINED;
      break;
    case 4:
      Source.DataSection = 1;
      break;
    case 5:
      Source.AnchorAddend = 1;
      break;
    case 6:
      Source.GetterAddend = 1;
      break;
    case 7:
      Source.DataValue = 12;
      break; // Eight-byte read exceeds BSS.
    case 8:
      Source.DataFlags |= COFF::IMAGE_SCN_MEM_EXECUTE;
      break;
    case 9:
      Source.Relocation = COFF::IMAGE_REL_AMD64_REL32_1;
      break;
    case 10:
      Source.Overlap = true;
      break;
    case 11:
      Source.FunctionTarget = true;
      break;
    case 12:
      Source.DataClass = COFF::IMAGE_SYM_CLASS_EXTERNAL;
      break;
    case 13:
      Source.DataClass = COFF::IMAGE_SYM_CLASS_WEAK_EXTERNAL;
      break;
    case 14:
      Source.DataFlags &= ~COFF::IMAGE_SCN_MEM_READ;
      break;
    case 15:
      Source.FunctionAnchor = false;
      break;
    case 16:
      Source.PrecedingOverlap = true;
      break;
    }
    const auto Modules = generateLocal(Source, Case != 0);
    ASSERT_GE(Modules.size(), Case == 1 || Case == 15 ? 1u : 2u) << Case;
    for (const auto &Module : Modules)
      if (std::any_of(
              Module.PublicNames.begin(), Module.PublicNames.end(),
              [](const FuncRef &Name) { return Name.Name == "getter"; }))
        EXPECT_TRUE(
            std::none_of(Module.References.begin(), Module.References.end(),
                         [](const FuncRef &Ref) { return Ref.IsLocalData; }))
            << Case;
  }
}

TEST(LocalDataEvidence, StaticFunctionAnchorsStillRequireUniqueDefinitions) {
  LocalObject Source;
  Source.AnchorClass = COFF::IMAGE_SYM_CLASS_STATIC;
  auto Modules = generateLocal(Source);
  ASSERT_EQ(Modules.size(), 2u);
  for (const auto &Module : Modules) {
    ASSERT_EQ(Module.References.size(), 1u);
    EXPECT_TRUE(Module.References[0].IsLocalData);
    EXPECT_EQ(Module.References[0].Name, "00000013:anchor");
  }

  Source.DuplicateAnchor = true;
  Modules = generateLocal(Source);
  ASSERT_EQ(Modules.size(), 2u);
  for (const auto &Module : Modules)
    EXPECT_TRUE(
        std::none_of(Module.References.begin(), Module.References.end(),
                     [](const FuncRef &Ref) { return Ref.IsLocalData; }));
}

TEST(LocalDataEvidence, LayoutUsesDefinedSectionCoordinatesAndSignedAddends) {
  for (unsigned Case = 0; Case < 5; ++Case) {
    LocalObject Source;
    Source.SeparateTarget = true;
    if (Case == 1 || Case == 2) {
      Source.DataValue = 8;
      Source.GetterValue = 0;
    }
    if (Case == 2)
      Source.GetterClass = COFF::IMAGE_SYM_CLASS_EXTERNAL;
    if (Case == 3) {
      Source.DataValue = 8;
      Source.GetterValue = 8;
      Source.GetterAddend = uint32_t(-8);
    }
    if (Case == 4) {
      Source.GetterValue = 0;
      Source.GetterAddend = 8;
    }
    const auto Modules = generateLocal(Source, true, true);
    ASSERT_EQ(Modules.size(), 2u);
    ASSERT_EQ(Modules[0].References.size(), 1u);
    EXPECT_EQ(Modules[0].References[0].Name, "00000013:anchor");
    ASSERT_EQ(Modules[1].References.size(), 1u) << Case;
    EXPECT_TRUE(Modules[1].References[0].IsLocalData);
    EXPECT_EQ(Modules[1].References[0].Name, Case == 0 || Case == 4
                                                 ? "00000013+00000008:anchor"
                                                 : "00000013-00000008:anchor");
    const auto Default = generateLocal(Source);
    ASSERT_EQ(Default.size(), 2u);
    EXPECT_TRUE(
        std::none_of(Default[1].References.begin(), Default[1].References.end(),
                     [](const FuncRef &Ref) { return Ref.IsLocalData; }));
  }
}

TEST(LocalDataEvidence, LayoutRefusesSplitUnknownAndOutOfBoundsCoordinates) {
  for (unsigned Case = 0; Case < 13; ++Case) {
    LocalObject Source;
    Source.SeparateTarget = true;
    switch (Case) {
    case 0:
      Source.DataFlags |= COFF::IMAGE_SCN_LNK_COMDAT;
      break;
    case 1:
      Source.DataFlags &= ~COFF::IMAGE_SCN_MEM_READ;
      break;
    case 2:
      Source.DataFlags |= COFF::IMAGE_SCN_MEM_EXECUTE;
      break;
    case 3:
      Source.GetterSection = COFF::IMAGE_SYM_UNDEFINED;
      break;
    case 4:
      Source.GetterClass = COFF::IMAGE_SYM_CLASS_WEAK_EXTERNAL;
      break;
    case 5:
      Source.GetterSection = 1;
      break;
    case 6:
      Source.AnchorAddend = 1;
      break;
    case 7:
      Source.GetterValue = 12;
      break;
    case 8:
      Source.GetterAddend = 1;
      break;
    case 9:
      Source.GetterAddend = uint32_t(-9);
      break;
    case 10:
      Source.DuplicateAnchor = true;
      break;
    case 11:
      Source.PrecedingOverlap = true;
      break;
    case 12:
      Source.SeparateTarget = false;
      Source.AuxiliaryTarget = true;
      break;
    }
    const auto Modules = generateLocal(Source, true, true);
    ASSERT_EQ(Modules.size(), 2u) << Case;
    EXPECT_TRUE(
        std::none_of(Modules[1].References.begin(), Modules[1].References.end(),
                     [](const FuncRef &Ref) { return Ref.IsLocalData; }))
        << Case;
  }
}
