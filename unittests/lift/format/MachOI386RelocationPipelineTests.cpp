//===- MachOI386RelocationPipelineTests.cpp - Mach-O i386 lift and recompile tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "MachOI386RelocationTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::macho_loader::detail;
using namespace neverd::macho_i386_test;

TEST_F(MachOI386Relocation, SectionDefinedFunctionAtZeroIsRetainedAndDetected) {
  auto Path = fixture("test_macho_i386.o");
  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  EXPECT_EQ(Img.Format, BinaryFormat::MachO);
  EXPECT_EQ(Img.Arch, Arch::X86);
  EXPECT_EQ(Img.Bits, Bitness::Bits32);
  EXPECT_TRUE(Img.IsRelocatable);

  auto Add =
      std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                   [](const Symbol &S) { return S.Name == "_i386_add"; });
  ASSERT_NE(Add, Img.Symbols.end());
  EXPECT_TRUE(Add->IsFunc);
  EXPECT_EQ(Add->Addr, 0u);
  for (llvm::StringRef DataName :
       {"_global_value", "_local_bias", "_readonly_value", "_i386_dispatch",
        "_i386_readonly_dispatch"}) {
    EXPECT_EQ(std::count_if(Img.Symbols.begin(), Img.Symbols.end(),
                            [&](const Symbol &S) {
                              return S.Name == DataName && S.IsFunc;
                            }),
              0u);
    EXPECT_EQ(
        std::count_if(Img.Exports.begin(), Img.Exports.end(),
                      [&](const Export &E) { return E.Name == DataName; }),
        0u);
  }

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);
  EXPECT_NE(std::find(Functions.begin(), Functions.end(),
                      std::make_pair(va_t(0), std::string("_i386_add"))),
            Functions.end());
  for (llvm::StringRef DataName :
       {"_global_value", "_local_bias", "_readonly_value", "_i386_dispatch",
        "_i386_readonly_dispatch"})
    EXPECT_EQ(
        std::count_if(Functions.begin(), Functions.end(),
                      [&](const auto &F) { return F.second == DataName; }),
        0u);
}

TEST_F(MachOI386Relocation,
       ZExtConstantDoesNotCreatePointerMirrorWithoutPointerSlots) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386.o"));
  auto Data = rawSectionLayout(Bytes, section_names::macho::Data);
  ASSERT_TRUE(Data.has_value());
  for (uint32_t Offset : {8u, 12u}) {
    auto RelocOffset = findRawRelocation(Bytes, *Data, Offset);
    ASSERT_TRUE(RelocOffset.has_value());
    writeRawRelocation(Bytes, *RelocOffset, Offset,
                       plainRelocationWord(2, false, 2, false,
                                           llvm::MachO::GENERIC_RELOC_VANILLA));
    writeSectionField(Bytes, *Data, Offset, Data->Address);
  }

  fs::path Path = writeMutation("zext_without_pointer_slots.o", Bytes);
  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_TRUE(ImgOrErr->CodePtrRelocSlots.empty());
  EXPECT_TRUE(ImgOrErr->DataPtrRelocSlots.empty());

  auto LLVM = liftToLLVMIR(Path);
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  EXPECT_EQ(LLVM.out.find("@__nd_codeptr_"), std::string::npos);
}

TEST_F(MachOI386Relocation,
       DispatchUsesRelinkablePointerToRecompiledZeroAddressFunction) {
  auto PICBytes = readBinaryFile(fixture("test_macho_i386.o"));
  auto PICObj = createMachOObject(PICBytes);
  auto PICText = PICObj ? findSection(*PICObj, section_names::macho::Text) : std::nullopt;
  auto PICData = rawSectionLayout(PICBytes, section_names::macho::Data);
  ASSERT_NE(PICObj, nullptr);
  ASSERT_TRUE(PICText.has_value());
  ASSERT_TRUE(PICData.has_value());

  std::optional<uint64_t> DispatchImmediateOffset;
  for (const auto &Reloc : relocations(*PICText)) {
    auto Info = PICObj->getRelocation(Reloc.getRawDataRefImpl());
    uint32_t Type = PICObj->getAnyRelocationType(Info);
    if ((Type == llvm::MachO::GENERIC_RELOC_SECTDIFF ||
         Type == llvm::MachO::GENERIC_RELOC_LOCAL_SECTDIFF) &&
        PICObj->getScatteredRelocationValue(Info) == PICData->Address + 8) {
      DispatchImmediateOffset = Reloc.getOffset();
      break;
    }
  }
  ASSERT_TRUE(DispatchImmediateOffset.has_value());

  auto PICImgOrErr = loadBinary(fixture("test_macho_i386.o"));
  ASSERT_TRUE(static_cast<bool>(PICImgOrErr))
      << llvm::toString(PICImgOrErr.takeError());
  const Section *LoadedText = PICImgOrErr->getSectionByName(section_names::macho::Text);
  const Section *LoadedData = PICImgOrErr->getSectionByName(section_names::macho::Data);
  ASSERT_NE(LoadedText, nullptr);
  ASSERT_NE(LoadedData, nullptr);
  EXPECT_EQ(PICImgOrErr->CodePtrRelocSlots.count(LoadedText->VA +
                                                 *DispatchImmediateOffset),
            0u);
  EXPECT_EQ(PICImgOrErr->CodePtrRelocSlots.count(LoadedData->VA), 0u);
  EXPECT_EQ(PICImgOrErr->CodePtrRelocSlots.count(LoadedData->VA + 4), 0u);
  EXPECT_NE(PICImgOrErr->CodePtrRelocSlots.count(LoadedData->VA + 8), 0u);
  EXPECT_NE(PICImgOrErr->WritableRelocDataAddrs.count(LoadedData->VA + 8), 0u);

  for (llvm::StringRef Name : {llvm::StringRef("test_macho_i386.o"),
                               llvm::StringRef("test_macho_i386_nopic.o")}) {
    SCOPED_TRACE(Name.str());
    auto LLVM = liftToLLVMIR(fixture(Name));
    ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
    EXPECT_NE(LLVM.out.find("@_i386_call_dispatch"), std::string::npos);
    EXPECT_NE(LLVM.out.find("ptrtoint (ptr @_i386_add to i32)"),
              std::string::npos);
  }
}

TEST_P(MachOI386Pipeline, CompletesLiftAndDecompilation) {
  const fs::path Path = fixture(GetParam().FixtureName);
  verifyAllStages(Path);
  verifyNoUnlifted(Path);

  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const Symbol *Global = findSymbol(*ImgOrErr, "_global_value");
  const Symbol *Local = findSymbol(*ImgOrErr, "_local_bias");
  ASSERT_NE(Global, nullptr);
  ASSERT_NE(Local, nullptr);

  auto LLVM = liftToLLVMIR(Path);
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  EXPECT_NE(LLVM.out.find("@_i386_add"), std::string::npos);
  EXPECT_NE(LLVM.out.find("@_i386_global_address"), std::string::npos);
  auto HasGlobalReference = [&](llvm::StringRef Name, va_t Address) {
    if (LLVM.out.find(Name.str()) != std::string::npos ||
        LLVM.out.find("i64 " + std::to_string(Address) + " to ptr") !=
            std::string::npos)
      return true;
    const Segment *Seg = ImgOrErr->getSegmentFor(Address);
    if (!Seg)
      return false;
    std::string Mirror = "@__nd_codeptr_" + llvm::utohexstr(Seg->VA);
    if (LLVM.out.find(Mirror) == std::string::npos)
      return false;
    uint64_t Offset = Address - Seg->VA;
    return Offset == 0 ||
           LLVM.out.find(Mirror + ", i64 " + std::to_string(Offset)) !=
               std::string::npos;
  };
  EXPECT_TRUE(HasGlobalReference("_global_value", Global->Addr));
  EXPECT_TRUE(HasGlobalReference("_local_bias", Local->Addr));

  auto Decompile = decompileToHighC(Path);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  auto CBytes = readBinaryFile(tmpFile("decompiled_high.c"));
  ASSERT_FALSE(CBytes.empty());
  llvm::StringRef C(reinterpret_cast<const char *>(CBytes.data()),
                    CBytes.size());
  EXPECT_TRUE(C.contains("i386_add"));
  EXPECT_TRUE(C.contains("i386_global_address"));
  EXPECT_FALSE(C.contains_insensitive("unlifted"));
}

INSTANTIATE_TEST_SUITE_P(
    ThinObjects, MachOI386Pipeline,
    ::testing::Values(MachOI386PipelineCase{"test_macho_i386.o", "PIC"},
                      MachOI386PipelineCase{"test_macho_i386_nopic.o",
                                            "NoPIC"}),
    [](const ::testing::TestParamInfo<MachOI386PipelineCase> &Info) {
      return Info.param.TestName;
    });

} // namespace
