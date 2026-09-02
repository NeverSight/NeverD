//===- PDBIdentityTests.cpp - PE/PDB identity policy tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/debug/PDBLoader.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/Object/CVDebugRecord.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/BinaryStreamRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace neverd;

namespace {

PDBBuildIdentity makeIdentity(uint8_t Seed, uint32_t Age = 1) {
  PDBBuildIdentity Identity;
  for (size_t I = 0; I < Identity.Guid.size(); ++I)
    Identity.Guid[I] = static_cast<uint8_t>(Seed + I);
  Identity.Age = Age;
  return Identity;
}

std::vector<uint8_t> makeRSDS(const PDBBuildIdentity &Identity,
                              llvm::StringRef Path = "fixture.pdb") {
  llvm::codeview::PDB70DebugInfo Header{};
  Header.CVSignature = static_cast<uint32_t>(llvm::OMF::Signature::PDB70);
  std::copy(Identity.Guid.begin(), Identity.Guid.end(), Header.Signature);
  Header.Age = Identity.Age;

  std::vector<uint8_t> Bytes(sizeof(Header) + Path.size() + 1, 0);
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), Path.data(), Path.size());
  return Bytes;
}

std::vector<uint8_t> makeMinimalCVSymbols(size_t Count) {
  const llvm::codeview::RecordPrefix Prefix(
      static_cast<uint16_t>(llvm::codeview::SymbolKind::S_END));
  std::vector<uint8_t> Bytes(Count * sizeof(Prefix));
  for (size_t I = 0; I < Count; ++I)
    std::memcpy(Bytes.data() + I * sizeof(Prefix), &Prefix, sizeof(Prefix));
  return Bytes;
}

std::filesystem::path safetyFixture(llvm::StringRef Name) {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "safety" / "fixtures" / "binaries" / Name.str();
}

llvm::Expected<BinaryImage> loadPEFixture(llvm::StringRef Name) {
  std::unique_ptr<Loader> ImageLoader = Loader::create(BinaryFormat::COFF);
  if (!ImageLoader)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "COFF loader is unavailable");
  return ImageLoader->load(safetyFixture(Name));
}

class ScratchDir {
public:
  ScratchDir() {
    llvm::SmallString<128> Created;
    std::error_code EC =
        llvm::sys::fs::createUniqueDirectory("neverd-pdb-identity", Created);
    EXPECT_FALSE(EC) << EC.message();
    Root = std::filesystem::path(Created.str().str());
  }

  ~ScratchDir() {
    if (!Root.empty())
      llvm::sys::fs::remove_directories(Root.string());
  }

  std::filesystem::path path(llvm::StringRef Name) const {
    return Root / Name.str();
  }

private:
  std::filesystem::path Root;
};

TEST(CodeViewRSDSParser, RecoversRawGuidAgeAndBoundedPath) {
  const PDBBuildIdentity Expected = makeIdentity(0x10, 7);
  std::vector<uint8_t> Bytes = makeRSDS(Expected, R"(C:\symbols\fixture.pdb)");

  auto Parsed = coff_loader::detail::parseCodeViewRSDS(Bytes);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->Identity, Expected);
  EXPECT_EQ(Parsed->Path, R"(C:\symbols\fixture.pdb)");
}

TEST(CodeViewRSDSParser, RejectsUnknownTruncatedAndUnterminatedRecords) {
  const PDBBuildIdentity Identity = makeIdentity(0x20);

  std::vector<uint8_t> Unknown = makeRSDS(Identity);
  Unknown[0] ^= 0xff;
  auto UnknownResult = coff_loader::detail::parseCodeViewRSDS(Unknown);
  EXPECT_FALSE(static_cast<bool>(UnknownResult));
  llvm::consumeError(UnknownResult.takeError());

  std::vector<uint8_t> Truncated(sizeof(llvm::codeview::PDB70DebugInfo) - 1, 0);
  auto TruncatedResult = coff_loader::detail::parseCodeViewRSDS(Truncated);
  EXPECT_FALSE(static_cast<bool>(TruncatedResult));
  llvm::consumeError(TruncatedResult.takeError());

  std::vector<uint8_t> Unterminated = makeRSDS(Identity);
  Unterminated.back() = 'x';
  auto UnterminatedResult =
      coff_loader::detail::parseCodeViewRSDS(Unterminated);
  EXPECT_FALSE(static_cast<bool>(UnterminatedResult));
  llvm::consumeError(UnterminatedResult.takeError());
}

TEST(CodeViewRSDSParser, RejectsZeroGuidAndZeroAge) {
  PDBBuildIdentity ZeroGuid;
  ZeroGuid.Age = 1;
  auto ZeroGuidResult =
      coff_loader::detail::parseCodeViewRSDS(makeRSDS(ZeroGuid));
  EXPECT_FALSE(static_cast<bool>(ZeroGuidResult));
  llvm::consumeError(ZeroGuidResult.takeError());

  PDBBuildIdentity ZeroAge = makeIdentity(0x30, 0);
  auto ZeroAgeResult =
      coff_loader::detail::parseCodeViewRSDS(makeRSDS(ZeroAge));
  EXPECT_FALSE(static_cast<bool>(ZeroAgeResult));
  llvm::consumeError(ZeroAgeResult.takeError());
}

TEST(CodeViewIdentityRegistry, IdenticalDuplicatesRemainUnique) {
  const PDBBuildIdentity Identity = makeIdentity(0x40, 2);
  coff_loader::detail::CodeViewIdentityRegistry Registry;
  Registry.observe({Identity, "z.pdb"});
  Registry.observe({Identity, "a.pdb"});

  ASSERT_EQ(Registry.state(), PDBIdentityState::Unique);
  ASSERT_TRUE(Registry.identity().has_value());
  EXPECT_EQ(*Registry.identity(), Identity);
  EXPECT_EQ(Registry.path(), "a.pdb");
}

TEST(CodeViewIdentityRegistry, EmptyRegistryHasNoIdentity) {
  coff_loader::detail::CodeViewIdentityRegistry Registry;
  EXPECT_EQ(Registry.state(), PDBIdentityState::Absent);
  EXPECT_FALSE(Registry.identity().has_value());
}

TEST(CodeViewIdentityRegistry, ConflictIsStickyAndOrderIndependent) {
  const coff_loader::detail::CodeViewRSDSRecord First{makeIdentity(0x50, 3),
                                                      "first.pdb"};
  const coff_loader::detail::CodeViewRSDSRecord Second{makeIdentity(0x60, 3),
                                                       "second.pdb"};
  for (const bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    coff_loader::detail::CodeViewIdentityRegistry Registry;
    Registry.observe(Reverse ? Second : First);
    Registry.observe(Reverse ? First : Second);
    Registry.observe(Reverse ? Second : First);
    EXPECT_EQ(Registry.state(), PDBIdentityState::Ambiguous);
    EXPECT_FALSE(Registry.identity().has_value());
  }
}

TEST(CodeViewIdentityRegistry, SameGuidWithDifferentAgeIsAmbiguous) {
  const PDBBuildIdentity First = makeIdentity(0x68, 1);
  PDBBuildIdentity Second = First;
  Second.Age = 2;

  coff_loader::detail::CodeViewIdentityRegistry Registry;
  Registry.observe({First, "same.pdb"});
  Registry.observe({Second, "same.pdb"});
  EXPECT_EQ(Registry.state(), PDBIdentityState::Ambiguous);
  EXPECT_FALSE(Registry.identity().has_value());
}

TEST(CodeViewIdentityRegistry, MalformedObservationIsStickyInEitherOrder) {
  const coff_loader::detail::CodeViewRSDSRecord Valid{makeIdentity(0x70),
                                                      "valid.pdb"};
  for (const bool MalformedFirst : {false, true}) {
    SCOPED_TRACE(MalformedFirst);
    coff_loader::detail::CodeViewIdentityRegistry Registry;
    if (MalformedFirst)
      Registry.observeMalformed();
    Registry.observe(Valid);
    if (!MalformedFirst)
      Registry.observeMalformed();
    EXPECT_EQ(Registry.state(), PDBIdentityState::Ambiguous);
    EXPECT_FALSE(Registry.identity().has_value());
  }
}

TEST(COFFRawBackedRangeResolver, MapsOneFullRangeToItsExactFileOffset) {
  const coff_loader::detail::RawBackedSectionRange Section{
      /*RVA=*/0x1000, /*VirtualSize=*/0x80, /*FileOffset=*/0x20,
      /*RawSize=*/0x60};
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      llvm::ArrayRef(Section), /*FileSize=*/0x100, /*RVA=*/0x1010,
      /*Size=*/0x10);
  ASSERT_TRUE(static_cast<bool>(Offset)) << llvm::toString(Offset.takeError());
  EXPECT_EQ(*Offset, 0x30u);
}

TEST(COFFRawBackedRangeResolver, RejectsDirectoryCrossingRawTail) {
  const coff_loader::detail::RawBackedSectionRange Section{
      /*RVA=*/0x1000, /*VirtualSize=*/0x80, /*FileOffset=*/0x20,
      /*RawSize=*/0x20};
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      llvm::ArrayRef(Section), /*FileSize=*/0x100, /*RVA=*/0x1018,
      /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(Offset));
  llvm::consumeError(Offset.takeError());
}

TEST(COFFRawBackedRangeResolver, RejectsPayloadCrossingVirtualBoundary) {
  const coff_loader::detail::RawBackedSectionRange Section{
      /*RVA=*/0x1000, /*VirtualSize=*/0x20, /*FileOffset=*/0x20,
      /*RawSize=*/0x60};
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      llvm::ArrayRef(Section), /*FileSize=*/0x100, /*RVA=*/0x1018,
      /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(Offset));
  llvm::consumeError(Offset.takeError());
}

TEST(COFFRawBackedRangeResolver, RejectsTruncatedDeclaredRawSection) {
  const coff_loader::detail::RawBackedSectionRange Section{
      /*RVA=*/0x1000, /*VirtualSize=*/0x20, /*FileOffset=*/0x50,
      /*RawSize=*/0x20};
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      llvm::ArrayRef(Section), /*FileSize=*/0x60, /*RVA=*/0x1000,
      /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(Offset));
  llvm::consumeError(Offset.takeError());
}

TEST(COFFRawBackedRangeResolver, RejectsOverlappingVirtualOwners) {
  const std::array<coff_loader::detail::RawBackedSectionRange, 2> Sections{{
      {/*RVA=*/0x1000, /*VirtualSize=*/0x80, /*FileOffset=*/0x20,
       /*RawSize=*/0x80},
      {/*RVA=*/0x1040, /*VirtualSize=*/0x80, /*FileOffset=*/0xa0,
       /*RawSize=*/0x80},
  }};
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      Sections, /*FileSize=*/0x140, /*RVA=*/0x1050, /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(Offset));
  llvm::consumeError(Offset.takeError());
}

TEST(CodeViewPayloadResolver, RequiresRVAAndRawToNameTheSameFileOffset) {
  const coff_loader::detail::RawBackedSectionRange Section{
      /*RVA=*/0x1000, /*VirtualSize=*/0x80, /*FileOffset=*/0x20,
      /*RawSize=*/0x80};
  std::vector<uint8_t> File(0xa0, 0x5a);

  auto SameContentDifferentOffset = coff_loader::detail::resolveCodeViewPayload(
      File, llvm::ArrayRef(Section), /*RVA=*/0x1010,
      /*RawFileOffset=*/0x40, /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(SameContentDifferentOffset));
  llvm::consumeError(SameContentDifferentOffset.takeError());

  std::fill(File.begin() + 0x40, File.begin() + 0x50, 0xa5);
  auto ConflictingContent = coff_loader::detail::resolveCodeViewPayload(
      File, llvm::ArrayRef(Section), /*RVA=*/0x1010,
      /*RawFileOffset=*/0x40, /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(ConflictingContent));
  llvm::consumeError(ConflictingContent.takeError());

  auto ExactOffset = coff_loader::detail::resolveCodeViewPayload(
      File, llvm::ArrayRef(Section), /*RVA=*/0x1010,
      /*RawFileOffset=*/0x30, /*Size=*/0x10);
  ASSERT_TRUE(static_cast<bool>(ExactOffset))
      << llvm::toString(ExactOffset.takeError());
  EXPECT_EQ(ExactOffset->data(), File.data() + 0x30);
}

TEST(PDBSymbolRecordIndex, AcceptsOnlyExactRecordStarts) {
  std::vector<uint8_t> Bytes = makeMinimalCVSymbols(2);
  llvm::BinaryByteStream Stream(Bytes, llvm::endianness::little);
  llvm::codeview::CVSymbolArray Records{llvm::BinaryStreamRef(Stream)};

  auto Indexed = pdb_loader_detail::indexSymbolRecords(Records);
  ASSERT_TRUE(static_cast<bool>(Indexed))
      << llvm::toString(Indexed.takeError());
  ASSERT_EQ(Indexed->size(), 2u);
  EXPECT_EQ((*Indexed)[0].Offset, 0u);
  EXPECT_EQ((*Indexed)[1].Offset, sizeof(llvm::codeview::RecordPrefix));
  EXPECT_NE(pdb_loader_detail::findSymbolAtExactOffset(*Indexed, 0), nullptr);
  EXPECT_EQ(pdb_loader_detail::findSymbolAtExactOffset(*Indexed, 1), nullptr);
}

TEST(PDBSymbolRecordIndex, TrailingMalformedRecordRejectsPartialIndex) {
  std::vector<uint8_t> Bytes = makeMinimalCVSymbols(1);
  Bytes.push_back(0xff);
  llvm::BinaryByteStream Stream(Bytes, llvm::endianness::little);
  llvm::codeview::CVSymbolArray Records{llvm::BinaryStreamRef(Stream)};

  auto Indexed = pdb_loader_detail::indexSymbolRecords(Records);
  EXPECT_FALSE(static_cast<bool>(Indexed));
  llvm::consumeError(Indexed.takeError());
}

TEST(PDBFunctionNameRegistry,
     ConflictingNamesAtOneAddressAreStickyAndOrderIndependent) {
  for (const bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    pdb_loader_detail::FunctionNameRegistry Registry;
    Registry.observe(0x140001000, Reverse ? "guarded_free" : "leaks_memory");
    Registry.observe(0x140001000, Reverse ? "leaks_memory" : "guarded_free");
    Registry.observe(0x140001000, Reverse ? "guarded_free" : "leaks_memory");

    EXPECT_EQ(Registry.state(0x140001000),
              pdb_loader_detail::FunctionNameState::Ambiguous);
    EXPECT_FALSE(Registry.name(0x140001000).has_value());
  }
}

TEST(PDBFunctionNameRegistry, IdenticalNamesRemainUnique) {
  pdb_loader_detail::FunctionNameRegistry Registry;
  Registry.observe(0x140001000, "leaks_memory");
  Registry.observe(0x140001000, "leaks_memory");

  EXPECT_EQ(Registry.state(0x140001000),
            pdb_loader_detail::FunctionNameState::Unique);
  ASSERT_TRUE(Registry.name(0x140001000).has_value());
  EXPECT_EQ(*Registry.name(0x140001000), "leaks_memory");
}

TEST(PDBIdentityIntegration, MatchingFixtureAuthenticatesNamesButNotExtents) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());
  ASSERT_EQ(ImageOr->DynInfo.CodeViewPDBIdentityState,
            PDBIdentityState::Unique);
  ASSERT_TRUE(ImageOr->DynInfo.CodeViewPDBIdentity.has_value());

  auto ContextOr =
      PDBDebugContext::load(safetyFixture("safety_cases_pe_x64.pdb"), *ImageOr);
  ASSERT_TRUE(static_cast<bool>(ContextOr))
      << llvm::toString(ContextOr.takeError());
  ASSERT_NE(*ContextOr, nullptr);
  EXPECT_TRUE((*ContextOr)->hasAuthenticatedImageIdentity());
  EXPECT_FALSE((*ContextOr)->hasExactObjectMetadataPrerequisites());
  EXPECT_FALSE((*ContextOr)->hasAuthenticatedFunctionSignatures());
  EXPECT_FALSE((*ContextOr)->hasAuthenticatedObjectExtents());
  EXPECT_TRUE((*ContextOr)->hasInfo());
  const std::vector<FunctionSym> Functions = (*ContextOr)->allFunctions();
  ASSERT_FALSE(Functions.empty());
  for (const FunctionSym &Function : Functions) {
    EXPECT_EQ(Function.Size, 0u)
        << "Phase A must not publish unauthenticated PDB code extents";
    EXPECT_FALSE(Function.ReturnType)
        << "Phase A must not consume an unvalidated PDB type graph";
  }
}

TEST(PDBIdentityIntegration, CrossArchitectureFixtureIsRejectedByGuidAndAge) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());

  auto ContextOr = PDBDebugContext::load(
      safetyFixture("safety_cases_pe_arm64.pdb"), *ImageOr);
  ASSERT_FALSE(static_cast<bool>(ContextOr));
  const std::string Error = llvm::toString(ContextOr.takeError());
  EXPECT_NE(Error.find("GUID/age does not match"), std::string::npos) << Error;
}

TEST(PDBIdentityIntegration, EitherGuidOrAgeMismatchRejectsTheCompanion) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());
  ASSERT_TRUE(ImageOr->DynInfo.CodeViewPDBIdentity.has_value());

  for (const bool ChangeAge : {false, true}) {
    SCOPED_TRACE(ChangeAge);
    BinaryImage Mutated = *ImageOr;
    if (ChangeAge)
      ++Mutated.DynInfo.CodeViewPDBIdentity->Age;
    else
      Mutated.DynInfo.CodeViewPDBIdentity->Guid.front() ^= 1;

    auto ContextOr = PDBDebugContext::load(
        safetyFixture("safety_cases_pe_x64.pdb"), Mutated);
    ASSERT_FALSE(static_cast<bool>(ContextOr));
    const std::string Error = llvm::toString(ContextOr.takeError());
    EXPECT_NE(Error.find("GUID/age does not match"), std::string::npos)
        << Error;
  }
}

TEST(PDBIdentityIntegration, ImageWithoutUniqueRSDSCannotUsePDBNames) {
  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::X64;

  auto ContextOr =
      PDBDebugContext::load(safetyFixture("safety_cases_pe_x64.pdb"), Image);
  ASSERT_FALSE(static_cast<bool>(ContextOr));
  const std::string Error = llvm::toString(ContextOr.takeError());
  EXPECT_NE(Error.find("no unique CodeView RSDS identity"), std::string::npos)
      << Error;
}

TEST(PDBIdentityIntegration, MatchingIdentityStillRequiresMachineAgreement) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());
  ImageOr->Arch = Arch::AArch64;

  auto ContextOr =
      PDBDebugContext::load(safetyFixture("safety_cases_pe_x64.pdb"), *ImageOr);
  ASSERT_FALSE(static_cast<bool>(ContextOr));
  const std::string Error = llvm::toString(ContextOr.takeError());
  EXPECT_NE(Error.find("machine does not match"), std::string::npos) << Error;
}

TEST(PDBIdentityIntegration, MatchingIdentityStillRequiresSectionAgreement) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());
  ASSERT_FALSE(ImageOr->Sections.empty());
  ++ImageOr->Sections.front().Size;

  auto ContextOr =
      PDBDebugContext::load(safetyFixture("safety_cases_pe_x64.pdb"), *ImageOr);
  ASSERT_FALSE(static_cast<bool>(ContextOr));
  const std::string Error = llvm::toString(ContextOr.takeError());
  EXPECT_NE(Error.find("section table does not match"), std::string::npos)
      << Error;
}

TEST(PDBIdentityDiscovery, ExplicitMismatchedPDBReportsIdentityError) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());

  DebugInfoRequest Request;
  Request.PDBPath = safetyFixture("safety_cases_pe_arm64.pdb");
  DebugInfoResult Result = loadDebugInfo(
      safetyFixture("safety_cases_pe_x64.exe"), *ImageOr, Request);
  EXPECT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(Result.Error.find("GUID/age does not match"), std::string::npos)
      << Result.Error;
}

TEST(PDBIdentityDiscovery, AutoSearchSkipsMismatchAndUsesLaterMatchingPDB) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());

  ScratchDir Dir;
  const std::filesystem::path Binary = Dir.path("probe.exe");
  const std::filesystem::path WrongRecordedPDB = Dir.path("recorded.pdb");
  ImageOr->DynInfo.PDBPath = WrongRecordedPDB.string();
  ASSERT_TRUE(std::filesystem::copy_file(
      safetyFixture("safety_cases_pe_x64.exe"), Binary));
  ASSERT_TRUE(std::filesystem::copy_file(
      safetyFixture("safety_cases_pe_arm64.pdb"), WrongRecordedPDB));
  ASSERT_TRUE(std::filesystem::copy_file(
      safetyFixture("safety_cases_pe_x64.pdb"), Dir.path("probe.pdb")));

  DebugInfoResult Result = loadDebugInfo(Binary, *ImageOr);
  ASSERT_TRUE(static_cast<bool>(Result)) << Result.Error;
  EXPECT_EQ(Result.Kind, DebugInfoKind::PDB);
  EXPECT_EQ(Result.Path.filename().string(), "probe.pdb");
}

} // namespace
