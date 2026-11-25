//===- PPC64ELFv1Tests.cpp - Descriptor and TOC evidence -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise descriptor admission, function discovery and TOC provenance with
/// target-endian ELF images.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/loader/DataSymbolBinding.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/loader/ELF/PowerPCABI.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ObjectYAML/yaml2obj.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>

using namespace neverd;

namespace {

constexpr va_t Text = 0x10000, OPD = 0x20000, Data = 0x30000;
constexpr va_t FirstTOC = Data + 0x8000, SecondTOC = FirstTOC + 8;

template <class T> std::string words(llvm::ArrayRef<T> Values, bool Little) {
  llvm::SmallVector<uint8_t> Bytes;
  for (T Value : Values)
    for (unsigned I = 0; I != sizeof(T); ++I)
      Bytes.push_back(Value >> (8 * (Little ? I : sizeof(T) - 1 - I)));
  return llvm::toHex(Bytes);
}

struct Fixture {
  bool Little;
  unsigned Flags = 1;
  va_t Entry = OPD;
  // ld r3,-32768(r2); blr, with a distinct TOC at the second entry.
  std::vector<uint32_t> Code = {0xe8628000, 0x4e800020, 0xe8628000, 0x4e800020};
  std::vector<va_t> Table = {Text, FirstTOC, 0, Text + 8, SecondTOC, Data};
  std::vector<va_t> DataWords = {37, 83};
  std::optional<va_t> SecondSymbol = OPD + 24;
  bool Shared = false;
  bool LegacyOPD = false;
  bool Interposed = false;
  std::vector<va_t> Dynamic;
  bool Sectionless = false;
  std::string yaml() const {
    auto Result =
        llvm::formatv(
            R"(--- !ELF
FileHeader:
  Class: ELFCLASS64
  Data: {0}
  Type: {6}
  Machine: EM_PPC64
  Entry: {1}
ProgramHeaders:
  - {{ Type: PT_LOAD, Flags: [ PF_R, PF_X ], VAddr: 0x10000, FirstSec: .text, LastSec: .text }
  - {{ Type: PT_LOAD, Flags: [ PF_R, PF_W ], VAddr: 0x20000, FirstSec: .opd, LastSec: .opd }
  - {{ Type: PT_LOAD, Flags: [ PF_R, PF_W ], VAddr: 0x30000, FirstSec: .data, LastSec: .data }
  - {{ Type: PT_LOAD, Flags: [ PF_R ], VAddr: 0x40000, FirstSec: .init_array, LastSec: .init_array }
{7}{8}Sections:
  - {{ Name: .text, Type: SHT_PROGBITS, Flags: [ SHF_ALLOC, SHF_EXECINSTR ], Address: 0x10000, Content: "{2}" }
  - {{ Name: .opd, Type: SHT_PROGBITS, Flags: [ SHF_ALLOC, SHF_WRITE ], Address: 0x20000, AddressAlign: 8, Content: "{3}" }
  - {{ Name: .data, Type: SHT_PROGBITS, Flags: [ SHF_ALLOC, SHF_WRITE ], Address: 0x30000, Content: "{4}" }
  - {{ Name: .init_array, Type: SHT_INIT_ARRAY, Flags: [ SHF_ALLOC ], Address: 0x40000, Content: "{5}" }
)",
            Little ? "ELFDATA2LSB" : "ELFDATA2MSB", Entry,
            words<uint32_t>(Code, Little),
            words<va_t>(Shared ? std::vector<va_t>(Table.size()) : Table,
                        Little),
            words<va_t>(DataWords, Little), words<va_t>({OPD}, Little),
            Shared || LegacyOPD ? "ET_DYN" : "ET_EXEC",
            Shared ? "  - { Type: PT_LOAD, Flags: [ PF_R ], VAddr: 0x50000, "
                     "FirstSec: .rela.dyn, LastSec: .rela.dyn }\n"
                   : "",
            Dynamic.empty() ? ""
                            : "  - { Type: PT_LOAD, Flags: [ PF_R ], VAddr: "
                              "0x60000, FirstSec: .tags, LastSec: .tags }\n"
                              "  - { Type: PT_DYNAMIC, Flags: [ PF_R ], VAddr: "
                              "0x60000, FirstSec: .tags, LastSec: .tags }\n")
            .str();
    if (!Dynamic.empty())
      Result +=
          llvm::formatv("  - {{ Name: .tags, Type: SHT_PROGBITS, Flags: [ "
                        "SHF_ALLOC ], Address: 0x60000, Content: \"{0}\" }\n",
                        words<va_t>(Dynamic, Little))
              .str();
    if (Shared) {
      Result +=
          "  - Name: .rela.dyn\n    Type: SHT_RELA\n    Flags: [ SHF_ALLOC ]\n"
          "    Address: 0x50000\n    Link: .dynsym\n    Relocations:\n";
      for (size_t I = 0; I < Table.size(); ++I) {
        if (!Table[I])
          continue;
        if (!I && Interposed)
          Result += "      - { Offset: 0x20000, Symbol: exposed, Type: "
                    "R_PPC64_ADDR64 }\n";
        else
          Result += llvm::formatv("      - {{ Offset: {0}, Type: "
                                  "R_PPC64_RELATIVE, Addend: {1} }\n",
                                  OPD + 8 * I, Table[I])
                        .str();
      }
      Result += "      - { Offset: 0x30000, Type: R_PPC64_RELATIVE, Addend: "
                "0x20000 }\n";
      Result += "DynamicSymbols:\n  - { Name: exposed, Type: STT_FUNC, "
                "Binding: STB_GLOBAL, Section: .text, Value: 0x10000 }\n";
    }
    if (SecondSymbol)
      Result += llvm::formatv(R"(Symbols:
  - {{ Name: first, Type: STT_FUNC, Binding: STB_GLOBAL, Section: .opd, Value: 0x20000, Size: 8 }
  - {{ Name: second, Type: STT_FUNC, Binding: STB_GLOBAL, Section: .opd, Value: {0}, Size: 8 }
)",
                              *SecondSymbol)
                    .str();
    return Result;
  }
  llvm::Expected<BinaryImage> load() const {
    llvm::SmallVector<char, 0> Storage;
    std::string Diagnostic;
    auto Object = llvm::yaml::yaml2ObjectFile(
        Storage, yaml(), [&](const llvm::Twine &T) { Diagnostic = T.str(); });
    if (!Object)
      return llvm::createStringError("fixture: %s", Diagnostic.c_str());
    llvm::support::endian::write32(
        Storage.data() + offsetof(llvm::object::ELF64LE::Ehdr, e_flags), Flags,
        Little ? llvm::endianness::little : llvm::endianness::big);
    if (Sectionless) {
      using Header = llvm::object::ELF64LE::Ehdr;
      std::fill_n(Storage.data() + offsetof(Header, e_shoff),
                  sizeof(Header::e_shoff), 0);
      std::fill_n(Storage.data() + offsetof(Header, e_shnum),
                  sizeof(Header::e_shnum), 0);
      std::fill_n(Storage.data() + offsetof(Header, e_shstrndx),
                  sizeof(Header::e_shstrndx), 0);
    }
    return ELFLoader().loadBuffer(Object->getMemoryBufferRef());
  }
};

TEST(PPC64ELFv1, DescriptorsBindNamesEntriesAndInitializersInBothOrders) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    F.Entry = *F.SecondSymbol;
    for (unsigned Flags : {0, 1}) {
      F.Flags = Flags;
      auto I = F.load();
      ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
      EXPECT_EQ(I->Arch, Little ? Arch::PPC64LEELFv1 : Arch::PPC64ELFv1);
      EXPECT_EQ(I->Entry, Text + 8);
      ASSERT_EQ(I->PPC64Descriptors.size(), 2u);
      EXPECT_EQ(I->PPC64Descriptors.at(OPD).TOC, FirstTOC);
      EXPECT_EQ(I->PPC64Descriptors.at(*F.SecondSymbol).Environment, Data);
      EXPECT_EQ(I->PPC64EntryContexts.at(Text + 8).TOC, SecondTOC);
      EXPECT_TRUE(I->isRuntimeFunctionAt(Text));
      EXPECT_FALSE(I->isFunctionEntryCandidateAt(OPD));
      EXPECT_EQ(I->DynInfo.InitArray, (std::vector<va_t>{Text}));
      for (const auto &S : I->Symbols)
        if (S.IsFunc)
          EXPECT_EQ(S.Addr, S.Name == "first" ? Text : Text + 8);
      ASSERT_EQ(I->Exports.size(), 2u);
      for (const auto &E : I->Exports)
        EXPECT_EQ(E.Addr, E.Name == "first" ? Text : Text + 8);
      // A stored function pointer still contains its descriptor address.
      EXPECT_EQ(readPtr(I->readVA(0x40000, 8), true, Little), OPD);
      EXPECT_EQ(getTargetRegInfo(I->Arch).CallStackArgumentBase, 112);
    }
  }
}

TEST(PPC64ELFv1, StrippedOverlappingDescriptorsKeepIndependentTOCs) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    F.Table.erase(F.Table.begin() + 2);
    F.SecondSymbol.reset();
    auto I = F.load();
    ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
    EXPECT_FALSE(I->PPC64Descriptors.at(OPD).Environment);
    EXPECT_EQ(I->PPC64Descriptors.at(OPD + 16).Entry, Text + 8);
    for (auto [Entry, Target] : {std::pair{Text, Data}, {Text + 8, Data + 8}}) {
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Entry};
      Options.LiftMode = true;
      Options.EmitDumpOutput = false;
      llvm::LLVMContext Ctx;
      auto R = Pipeline().run(*I, Ctx, Options);
      ASSERT_TRUE(R.Success) << R.Error;
      ASSERT_TRUE(R.LlvmModule);
      EXPECT_FALSE(llvm::verifyModule(*R.LlvmModule, &llvm::errs()));
      // Clang/LLVM currently describe the LE function-pointer alignment with
      // Fn32 even when code generation uses the ELFv1 descriptor convention.
      EXPECT_TRUE(llvm::StringRef(R.LlvmModule->getDataLayoutStr())
                      .contains(Little ? "Fn32" : "Fi64"));
      ASSERT_EQ(R.MedFuncs.size(), 1u);
      bool Found = false;
      for (const auto &B : R.MedFuncs.front().Blocks)
        for (const auto &Op : B.Ops)
          Found |= Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
                   Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == Target;
      EXPECT_TRUE(Found) << Target;
    }
  }
}

TEST(PPC64ELFv1, AppliesLocalDescriptorRelocationsBeforeResolvingSymbols) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    F.Shared = true;
    auto I = F.load();
    ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
    EXPECT_EQ(I->PPC64Descriptors.at(OPD).Entry, Text);
    EXPECT_EQ(I->PPC64Descriptors.at(OPD + 24).TOC, SecondTOC);
    EXPECT_EQ(I->PPC64Descriptors.at(OPD + 24).Environment, Data);
    EXPECT_TRUE(I->CodePtrRelocSlots.contains(Data));
    EXPECT_FALSE(I->DataPtrRelocSlots.contains(Data));
    EXPECT_EQ(readPtr(I->readVA(Data, 8), true, Little), OPD);
    EXPECT_EQ(collectPointerVariableCallees(*I).at(Data).LocalEntry, Text);
    EXPECT_FALSE(
        elf_loader::isELFSlotBinding(I->Arch, llvm::ELF::R_PPC64_JMP_SLOT));
    F.Interposed = true;
    I = F.load();
    ASSERT_FALSE(bool(I));
    EXPECT_NE(llvm::toString(I.takeError())
                  .find("unresolved PPC64 descriptor relocation"),
              std::string::npos);
  }
}

TEST(PPC64ELFv1, DynamicTagsLocateSectionlessTablesAndMustAgreeWithSections) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    F.Flags = 0;
    F.LegacyOPD = true;
    F.Dynamic = {ppc_elf::DynamicOPD,
                 OPD,
                 ppc_elf::DynamicOPDSize,
                 F.Table.size() * ppc_elf::WordSize,
                 0,
                 0};
    for (bool Sectionless : {false, true}) {
      F.Sectionless = Sectionless;
      auto I = F.load();
      ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
      EXPECT_EQ(I->Entry, Text);
      EXPECT_EQ(I->PPC64Descriptors.at(OPD + 24).TOC, SecondTOC);
    }
    F.Sectionless = false;
    F.Dynamic[1] += ppc_elf::WordSize;
    auto I = F.load();
    ASSERT_FALSE(bool(I));
    EXPECT_NE(llvm::toString(I.takeError()).find("conflicting PPC64"),
              std::string::npos);
  }
}

TEST(PPC64ELFv1, SplitTOCDisplacementsResolveDataAndCallableValues) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    F.Code = {0x3c820001, 0xe8648000, 0x4e800020,  // addis; ld; blr
              0x3c62ffff, 0x38638000, 0x4e800020}; // addis; addi; blr
    F.Table = {Text, Data - 0x8000, 0, Text + 12, OPD + 0x18000, 0};
    auto I = F.load();
    ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
    Decoder D;
    ASSERT_TRUE(D.init(*I));
    for (auto [Entry, Value] : {std::pair{Text, Data}, {Text + 12, Text}}) {
      auto Low = CFGBuilder().build(*I, D, Entry, I->getFunctionNameAt(Entry));
      LowToMedConverter Convert;
      Convert.setBinaryImage(&*I);
      auto M = Convert.convert(Low, I->Arch, BinaryFormat::ELF);
      const bool Callable = Entry != Text;
      bool Found = false;
      for (const auto &B : M.Blocks)
        for (const auto &Op : B.Ops)
          Found |= Op.Opcode == (Callable ? NdOp::COPY : NdOp::LOAD) &&
                   Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
                   Op.Inputs[0].ConstVal == Value &&
                   Op.Inputs[0].Provenance ==
                       (Callable ? ConstantAddressProvenance::CodeAddress
                                 : ConstantAddressProvenance::DataAddress);
      EXPECT_TRUE(Found) << Entry;
    }
  }
}

TEST(PPC64ELFv1, ConflictingTOCAliasesCannotSeedOneBody) {
  Fixture F{false};
  F.Table[3] = Text;
  auto I = F.load();
  ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
  EXPECT_FALSE(I->PPC64EntryContexts.at(Text).TOC);
  Decoder D;
  ASSERT_TRUE(D.init(*I));
  auto L = CFGBuilder().build(*I, D, Text, "aliased");
  LowToMedConverter Convert;
  Convert.setBinaryImage(&*I);
  EXPECT_THROW(Convert.convert(L, I->Arch, BinaryFormat::ELF),
               LowToMedConversionError);
}

TEST(PPC64ELFv1, SourceCallsRequireTOCAndInitializerEvidence) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    auto RejectCall = [&](llvm::StringRef Reason) {
      auto I = F.load();
      ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
      Decoder D;
      ASSERT_TRUE(D.init(*I));
      auto L = CFGBuilder().build(*I, D, Text, "caller");
      LowToMedConverter Convert;
      Convert.setBinaryImage(&*I);
      try {
        Convert.convert(L, I->Arch, BinaryFormat::ELF);
        FAIL() << "accepted an unproven call";
      } catch (const LowToMedConversionError &E) {
        EXPECT_TRUE(llvm::StringRef(E.what()).contains(Reason)) << E.what();
      }
    };
    F.Code = {0x48000009, 0x4e800020, 0xe8628000, 0x4e800020};
    RejectCall("TOC contexts");
    // ld r12,-32768(r2) loads an initialized variable; bctrl uses its
    // pointer. Without a relocation its native initializer cannot be rebuilt.
    F.DataWords[0] = OPD;
    F.Table = {Text, FirstTOC, 0};
    F.SecondSymbol.reset();
    F.Code = {0xe9828000, 0xe84c0008, 0xe96c0010, 0xe98c0000,
              0x7d8903a6, 0x4e800421, 0x4e800020};
    RejectCall("initializer");
  }
}

TEST(PPC64ELFv1, IndirectCallRequiresMatchingEntryTOCAndEnvironment) {
  // Clang's callback sequence. Move the descriptor entry to CTR after saving
  // the caller's TOC and loading the two registers the callee receives.
  for (bool Little : {false, true}) {
    Fixture F{Little};
    F.Table = {Text, FirstTOC, 0};
    F.SecondSymbol.reset();
    F.Code = {0x7c0802a6, 0xf821ff91, 0xf8010080, 0xf8410028,
              0xe8430008, 0xe9630010, 0xe8630000, 0x7c6903a6,
              0x7c832378, 0x4e800421, 0xe8410028, 0x38630001,
              0x38210070, 0xe8010010, 0x7c0803a6, 0x4e800020};
    for (bool Mismatched : {false, true}) {
      if (Mismatched)
        F.Code[4] = 0xe8440008; // ld r2,8(r4), a different descriptor.
      auto I = F.load();
      ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
      Decoder D;
      ASSERT_TRUE(D.init(*I));
      auto L = CFGBuilder().build(*I, D, Text, "callback");
      ASSERT_TRUE(L.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      Convert.setBinaryImage(&*I);
      if (Mismatched) {
        EXPECT_THROW(Convert.convert(L, I->Arch, BinaryFormat::ELF),
                     LowToMedConversionError);
      } else {
        auto M = Convert.convert(L, I->Arch, BinaryFormat::ELF);
        unsigned Calls = 0;
        for (const auto &B : M.Blocks)
          for (const auto &Op : B.Ops)
            if (Op.Opcode == NdOp::INDIR_CALL) {
              ++Calls;
              ASSERT_GT(Op.NumInputs, 0);
              EXPECT_EQ(Op.Inputs[0].RegOff,
                        getTargetRegInfo(I->Arch).IntParamRegs.front());
              EXPECT_TRUE(Op.Inputs[0].Kind == MedVar::Param ||
                          (Op.Inputs[0].Kind == MedVar::Reg &&
                           Op.Inputs[0].SSAVer == 0));
            }
        EXPECT_EQ(Calls, 1u);
      }
    }
  }
}

// One undefined dynamic function, with the empty GNU hash buckets GNU ld
// emits for this case. Only PT_DYNAMIC locates these runtime tables.
Fixture pltFixture(bool Little) {
  using namespace llvm::ELF;
  Fixture F{Little};
  F.SecondSymbol.reset();
  F.Table = {Text, FirstTOC, 0};
  F.DataWords = {37, 0, 0, 0};
  // The caller restores r2 before reading its own global. The linkage
  // stub starts at Text+64 and reads the inline descriptor at Data+8.
  F.Code = {0x7c0802a6, 0xf821ff91, 0xf8010080, 0x38630003,
            0x48000031, 0xe8410028, 0xe8828000, 0x7c632214,
            0x38210070, 0xe8010010, 0x7c0803a6, 0x4e800020};
  F.Code.resize(16);
  F.Code.insert(F.Code.end(),
                {0xf8410028, 0xe9828008, 0x7d8903a6, 0xe8428010, 0x4e800420});
  constexpr va_t Tags = 0x60000, Sym = 0x100, Str = 0x140, Rel = 0x180,
                 Hash = 0x1c0;
  F.Dynamic = {
      DT_STRTAB,   Tags + Str, DT_STRSZ,    8,           DT_SYMTAB, Tags + Sym,
      DT_SYMENT,   24,         DT_GNU_HASH, Tags + Hash, DT_JMPREL, Tags + Rel,
      DT_PLTRELSZ, 24,         DT_PLTREL,   DT_RELA,     DT_NULL,   0};
  F.Dynamic.resize((Hash + 32) / 8);
  const auto ReadWord = [&](const void *P) -> uint64_t {
    return Little ? llvm::support::endian::read64le(P)
                  : llvm::support::endian::read64be(P);
  };
  F.Dynamic[Str / 8] = ReadWord("\0remote\0");
  F.Dynamic[(Sym + 24) / 8] = Little ? 0x1200000001ULL : 0x112000000ULL;
  F.Dynamic[Rel / 8] = Data + 8;
  F.Dynamic[Rel / 8 + 1] = (uint64_t(1) << 32) | R_PPC64_JMP_SLOT;
  F.Dynamic[Hash / 8] = (uint64_t(1) << 32) | 1;
  F.Dynamic[Hash / 8 + 1] = Little ? 1 : uint64_t(1) << 32;
  return F;
}

TEST(PPC64ELFv1, PLTCallRequiresTheRuntimeDescriptorAndItsReachingTOC) {
  for (bool Little : {false, true}) {
    for (unsigned Variant = 0; Variant < 5; ++Variant) {
      Fixture F = pltFixture(Little);
      if (Variant == 1)
        F.Code[3] = 0x38420008; // Change the actual r2 before the call.
      if (Variant == 2)
        F.Code[19] = 0xe8428018; // Load the environment as the TOC.
      if (Variant == 3)
        F.Dynamic[0x180 / 8 + 1] =
            (uint64_t(1) << 32) | llvm::ELF::R_PPC64_GLOB_DAT;
      if (Variant == 4)
        F.Dynamic[0x180 / 8] += 1;
      auto I = F.load();
      if (Variant == 4) {
        ASSERT_FALSE(bool(I));
        EXPECT_TRUE(llvm::StringRef(llvm::toString(I.takeError()))
                        .contains("PLT descriptor extent"));
        continue;
      }
      ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
      const auto *Import = I->findImportStubAt(Text + 64);
      if (Variant <= 1) {
        ASSERT_NE(Import, nullptr);
        EXPECT_EQ(Import->Name, "remote");
        ASSERT_EQ(I->Relocations.size(), 1u);
        ASSERT_TRUE(I->Relocations.front().ELF);
        EXPECT_EQ(I->Relocations.front().ELF->Source,
                  ELFRelocationSource::ProgramDynamicTable);
        EXPECT_EQ(I->findImportAt(Data + 8), nullptr);
        EXPECT_FALSE(
            I->collectImportStorageSlot(Data + 8).Slots.count(Data + 8));
      } else
        EXPECT_EQ(Import, nullptr);
      Decoder D;
      ASSERT_TRUE(D.init(*I));
      auto L = CFGBuilder().build(*I, D, Text, "caller");
      LowToMedConverter Convert;
      Convert.setBinaryImage(&*I);
      if (Variant) {
        EXPECT_THROW(Convert.convert(L, I->Arch, BinaryFormat::ELF),
                     LowToMedConversionError);
        continue;
      }
      auto Med = Convert.convert(L, I->Arch, BinaryFormat::ELF);
      bool CallsImport = false, ReadsCallerData = false;
      for (const auto &B : Med.Blocks)
        for (const auto &Op : B.Ops)
          if (Op.NumInputs && Op.Inputs[0].isConst()) {
            CallsImport |=
                Op.Opcode == NdOp::CALL && Op.Inputs[0].ConstVal == Text + 64;
            ReadsCallerData |=
                Op.Opcode == NdOp::LOAD && Op.Inputs[0].ConstVal == Data;
          }
      EXPECT_TRUE(CallsImport);
      EXPECT_TRUE(ReadsCallerData);
    }
  }
}

TEST(PPC64ELFv1, PLTChecksFarDescriptorFieldsAndLazyRelocationOrdinal) {
  for (bool Little : {false, true}) {
    for (bool Correct : {true, false}) {
      Fixture Far = pltFixture(Little);
      // The descriptor crosses a signed low-half boundary. GNU ld adjusts
      // the base before loading its TOC and static chain.
      Far.Table[1] = Data + 8 - 0x17ff8;
      Far.Code.resize(16);
      Far.Code.insert(Far.Code.end(),
                      {0xf8410028, 0x3d620001, 0xe98b7ff8, 0x396b7ff8,
                       0x7d8903a6, 0x7d826278, 0x7d6b1214, 0xe84b0008,
                       Correct ? 0xe96b0010U : 0xe96b0018U, 0x4e800420});
      auto I = Far.load();
      ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
      EXPECT_EQ(I->findImportStubAt(Text + 64) != nullptr, Correct);

      Fixture Lazy = pltFixture(Little);
      Lazy.Code.resize(20);
      Lazy.Code.insert(Lazy.Code.end(),
                       {0x28220000, 0x4ce20420, 0x48000008, 0, 0x38000000});
      // Ordinal zero starts 32 bytes after DT_PPC64_GLINK.
      Lazy.Dynamic[16] = 0x70000000;
      Lazy.Dynamic[17] = Text + (Correct ? 64 : 72);
      auto L = Lazy.load();
      ASSERT_TRUE(bool(L)) << llvm::toString(L.takeError());
      EXPECT_EQ(L->findImportStubAt(Text + 64) != nullptr, Correct);
    }
  }
}

TEST(PPC64ELFv1, CallerSavedTOCRequiresAReachingFrameWord) {
  for (bool Little : {false, true})
    for (unsigned Variant = 0; Variant != 3; ++Variant) {
      Fixture F = pltFixture(Little);
      F.Code[4] = 0x48000035; // Enter after the stub's save.
      F.Code[3] = Variant == 0 ? 0xf8410028 : 0x60000000;
      if (Variant == 2) {
        F.Code[2] = 0xf8410028; // Save the full word, then overwrite part.
        F.Code[3] = 0x9061002c;
      }
      auto Image = F.load();
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      ASSERT_NE(Image->findImportStubAt(Text + 68), nullptr);
      Decoder D;
      ASSERT_TRUE(D.init(*Image));
      auto Low = CFGBuilder().build(*Image, D, Text, "caller");
      LowToMedConverter Convert;
      Convert.setBinaryImage(&*Image);
      if (Variant) {
        EXPECT_THROW(Convert.convert(Low, Image->Arch, BinaryFormat::ELF),
                     LowToMedConversionError);
        continue;
      }
      auto Med = Convert.convert(Low, Image->Arch, BinaryFormat::ELF);
      bool ReadsCallerData = false;
      for (const auto &B : Med.Blocks)
        for (const auto &Op : B.Ops)
          ReadsCallerData |= Op.Opcode == NdOp::LOAD &&
                             Op.Inputs[0].isConst() &&
                             Op.Inputs[0].ConstVal == Data;
      EXPECT_TRUE(ReadsCallerData);
    }
}

TEST(PPC64ELFv1, SectionlessPLTRequiresDisjointRuntimeDescriptors) {
  for (bool Little : {false, true}) {
    Fixture F = pltFixture(Little);
    F.Sectionless = true;
    F.Dynamic[16] = ppc_elf::DynamicOPD;
    F.Dynamic[17] = OPD;
    F.Dynamic[18] = ppc_elf::DynamicOPDSize;
    F.Dynamic[19] = 24;
    auto Image = F.load();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    const auto *Import = Image->findImportStubAt(Text + 64);
    ASSERT_NE(Import, nullptr);
    EXPECT_EQ(Import->Name, "remote");

    F.DataWords.resize(8);
    F.Dynamic[13] = 48; // Two records in DT_JMPREL.
    F.Dynamic[0x198 / 8] = Data + 16;
    for (auto Type :
         {llvm::ELF::R_PPC64_JMP_SLOT, llvm::ELF::R_PPC64_GLOB_DAT}) {
      F.Dynamic[0x198 / 8 + 1] = (uint64_t(1) << 32) | Type;
      auto Invalid = F.load();
      ASSERT_FALSE(bool(Invalid));
      EXPECT_TRUE(llvm::StringRef(llvm::toString(Invalid.takeError()))
                      .contains("PPC64 PLT descriptor"));
    }
  }
}

TEST(PPC64ELFv1, DirectCallUsesTheTOCEstablishedAtTheBranch) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    F.Code = {0x7c0802a6, 0xf821ff91, 0xf8010080, 0xf8410028, 0x38420008,
              0x4800001d, 0xe8410028, 0x38210070, 0xe8010010, 0x7c0803a6,
              0x4e800020, 0,          0xe8628000, 0x4e800020};
    F.Table = {Text, FirstTOC, 0, Text + 48, SecondTOC, 0};
    for (bool Correct : {true, false}) {
      if (!Correct)
        F.Code[4] = 0x38420004;
      auto I = F.load();
      ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
      Decoder D;
      ASSERT_TRUE(D.init(*I));
      auto L = CFGBuilder().build(*I, D, Text, "caller");
      LowToMedConverter Convert;
      Convert.setBinaryImage(&*I);
      if (Correct)
        EXPECT_NO_THROW(Convert.convert(L, I->Arch, BinaryFormat::ELF));
      else
        EXPECT_THROW(Convert.convert(L, I->Arch, BinaryFormat::ELF),
                     LowToMedConversionError);
    }
  }
}

TEST(PPC64ELFv1, RejectsMalformedAndConflictingDescriptorEvidence) {
  for (bool Little : {false, true}) {
    Fixture F{Little};
    auto Reject = [](const Fixture &Bad) {
      auto I = Bad.load();
      ASSERT_FALSE(bool(I));
      EXPECT_TRUE(
          llvm::StringRef(llvm::toString(I.takeError())).starts_with("elf:"));
    };
    F.Flags = 2;
    Reject(F);
    F.Flags = 1;
    F.Entry = OPD + 8;
    Reject(F);
    F.Entry = OPD;
    F.Table[0] = Data;
    Reject(F);
    F.Table[0] = Text + 2;
    Reject(F);
    F.Table = {Text};
    Reject(F);
  }
}
} // namespace
