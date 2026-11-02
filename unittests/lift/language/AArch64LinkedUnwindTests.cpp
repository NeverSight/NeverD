//===- AArch64LinkedUnwindTests.cpp - Real Apple unwind transport --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Assemble and link declared arm64e unwind contracts, then query the public
/// loader model. This does not execute code or certify native unwinding.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/Loader.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::dwarf_eh;

#define NEVERD_A64_LINKED_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define NEVERD_A64_LINKED_TEXT(Name, Text) constexpr char Name[] = Text;
#include "AArch64LinkedUnwindFixtures.def"

class AArch64LinkedUnwind : public ::testing::Test {
protected:
  std::filesystem::path Directory;
  std::string Xcrun;
  std::string Diagnostic;

  std::filesystem::path file(const char *Name) const {
    return Directory / Name;
  }

  static std::string read(const std::filesystem::path &Path) {
    auto Buffer = llvm::MemoryBuffer::getFile(Path.string());
    return Buffer ? (*Buffer)->getBuffer().str() : Buffer.getError().message();
  }

  void write(const char *Name, llvm::StringRef Text) {
    std::error_code Error;
    llvm::raw_fd_ostream Output(file(Name).string(), Error);
    ASSERT_FALSE(Error) << Error.message();
    Output << Text;
    Output.close();
    ASSERT_FALSE(Output.has_error());
  }

  bool run(const std::vector<std::string> &Arguments) {
    std::vector<llvm::StringRef> Args{Xcrun};
    Args.insert(Args.end(), Arguments.begin(), Arguments.end());
    const auto Stdout = file(kStdout).string();
    const auto Stderr = file(kStderr).string();
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Stdout,
                                                        Stderr};
    std::string Error;
    const int Status = llvm::sys::ExecuteAndWait(
        Xcrun, Args, std::nullopt, Redirects, kToolTimeoutSeconds, 0, &Error);
    Diagnostic = Error + read(file(kStdout)) + read(file(kStderr));
    return Status == 0;
  }

  void SetUp() override {
#if !defined(__APPLE__)
    GTEST_SKIP() << kHostUnavailable;
#else
    auto Tool = llvm::sys::findProgramByName(kXcrun);
    if (!Tool)
      GTEST_SKIP() << kHostUnavailable << Tool.getError().message();
    Xcrun = *Tool;
    llvm::SmallString<kPathCapacity> Path;
    const auto Error =
        llvm::sys::fs::createUniqueDirectory(kTemporaryPrefix, Path);
    ASSERT_FALSE(Error) << Error.message();
    Directory = Path.str().str();
    ASSERT_NO_FATAL_FAILURE(write(kProbeSourceFile, kProbeSource));
    if (!run({kClang, kTarget, kTriple, kDynamicLibrary,
              file(kProbeSourceFile).string(), kOutput,
              file(kProbeImageFile).string()}))
      GTEST_SKIP() << kToolchainUnavailable << Diagnostic;
#endif
  }

  void TearDown() override {
    if (!Directory.empty() && !HasFailure()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Directory, Ignored);
    }
  }

  void link(llvm::StringRef Source) {
    ASSERT_NO_FATAL_FAILURE(write(kSourceFile, Source));
    ASSERT_TRUE(
        run({kClang, kTarget, kTriple, kCompile, file(kSourceFile).string(),
             kOutput, file(kObjectFile).string()}))
        << Diagnostic;
    ASSERT_TRUE(
        run({kClang, kTarget, kTriple, kDynamicLibrary,
             file(kObjectFile).string(), kOutput, file(kImageFile).string()}))
        << Diagnostic;
  }

  void checkDwarf(bool Absolute) {
    ASSERT_NO_FATAL_FAILURE(
        link(std::string(kDwarfPrefix) +
             (Absolute ? kAbsoluteSigning : kLegacySigning) + kDwarfSuffix));
    auto Reader = Loader::create(BinaryFormat::MachO);
    auto Image = Reader->load(file(kImageFile));
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    ASSERT_TRUE(Image->isArm64e());
    const auto *Symbol = Image->findSymbol(kSymbol);
    ASSERT_NE(Symbol, nullptr);
    const auto *Function = Image->ExceptionMetadata.findFunction(Symbol->Addr);
    ASSERT_NE(Function, nullptr);
    ASSERT_EQ(Function->ParseStatus, ExceptionParseStatus::Complete);
    ASSERT_TRUE(Function->Dwarf);
    ASSERT_TRUE(Function->Compact);
    EXPECT_EQ(Function->Compact->Kind, CompactUnwindKind::DwarfFDE);
    EXPECT_EQ(Function->Compact->DwarfFDEOffset,
              Function->Dwarf->SectionOffset);
    const auto &FDE = *Function->Dwarf;
    EXPECT_EQ(FDE.InitialLocation, Symbol->Addr);
    EXPECT_EQ(FDE.AddressRange, kDwarfCodeBytes);
    const auto *CIE = Image->ExceptionMetadata.findCIE(FDE.CIESectionOffset);
    ASSERT_NE(CIE, nullptr);
    EXPECT_TRUE(CIE->HasPointerAuth);
    EXPECT_FALSE(CIE->UsesNonHintPAuthLR);
    const auto *Code = Image->readVA(Symbol->Addr, kAbsoluteStateOffset);
    ASSERT_NE(Code, nullptr);
    EXPECT_EQ(readLE<uint32_t>(Code), kPACM);
    EXPECT_EQ(readLE<uint32_t>(Code + kSigningOffset), kPACIBSP);
    const uint64_t StateOffset =
        Absolute ? kAbsoluteStateOffset : kSigningOffset;
    unsigned SigningRules = 0;
    for (const auto &Instruction : FDE.Instructions) {
      if (Instruction.Kind !=
          (Absolute ? CFIOpKind::SetRAState : CFIOpKind::NegateRAStateWithPC))
        continue;
      ++SigningRules;
      EXPECT_EQ(Instruction.CodeOffset, StateOffset);
      if (Absolute) {
        EXPECT_EQ(Instruction.RAState, kPCSigningState);
        EXPECT_EQ(Instruction.Offset, kSigningDisplacement);
      }
    }
    EXPECT_EQ(SigningRules, 1u);
    const auto Before = evaluateAArch64ReturnAddressState(
        Image->Arch, *CIE, FDE, Symbol->Addr + StateOffset - 1);
    ASSERT_EQ(Before.Status, AArch64RAQueryStatus::Known);
    ASSERT_TRUE(Before.State);
    EXPECT_EQ(Before.State->Kind, AArch64RASigningKind::Unsigned);
    EXPECT_FALSE(Before.State->SigningPC);
    for (uint64_t Offset : {StateOffset, kDwarfCodeBytes - 1}) {
      const auto Query = evaluateAArch64ReturnAddressState(
          Image->Arch, *CIE, FDE, Symbol->Addr + Offset);
      ASSERT_EQ(Query.Status, AArch64RAQueryStatus::Known);
      ASSERT_TRUE(Query.State);
      EXPECT_EQ(Query.State->Kind, AArch64RASigningKind::SPAndPC);
      EXPECT_EQ(Query.State->Key, AArch64RAKey::B);
      EXPECT_EQ(Query.State->Recipe, AArch64RARecipe::HintPACM);
      EXPECT_EQ(Query.State->SigningPC, Symbol->Addr + kSigningOffset);
    }
    const auto End = evaluateAArch64ReturnAddressState(
        Image->Arch, *CIE, FDE, Symbol->Addr + kDwarfCodeBytes);
    EXPECT_EQ(End.Status, AArch64RAQueryStatus::OutOfRange);
    EXPECT_FALSE(End.State);
  }
};

TEST_F(AArch64LinkedUnwind, LinkerPreservesAnExplicitCompactPAuthLRRecipe) {
  ASSERT_NO_FATAL_FAILURE(link(kCompactSource));
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Image = Reader->load(file(kImageFile));
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->isArm64e());
  const auto *Symbol = Image->findSymbol(kSymbol);
  ASSERT_NE(Symbol, nullptr);
  const auto *Function = Image->ExceptionMetadata.findFunction(Symbol->Addr);
  ASSERT_NE(Function, nullptr);
  ASSERT_EQ(Function->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(Function->Compact);
  const auto &Compact = *Function->Compact;
  EXPECT_EQ(Compact.NativeEncoding, kCompactEncoding);
  EXPECT_EQ(Compact.SemanticStatus, CompactUnwindSemanticStatus::Complete);
  EXPECT_EQ(Compact.Kind, CompactUnwindKind::FramePointer);
  EXPECT_EQ(Compact.CodeRange.Begin, Symbol->Addr);
  EXPECT_EQ(Compact.CodeRange.End, Symbol->Addr + kCompactCodeBytes);
  ASSERT_TRUE(Compact.ReturnAddressSigningOffset);
  EXPECT_EQ(*Compact.ReturnAddressSigningOffset, kSigningOffset);
  const auto *Code = Image->readVA(Compact.CodeRange.Begin +
                                       *Compact.ReturnAddressSigningOffset,
                                   sizeof(uint32_t));
  ASSERT_NE(Code, nullptr);
  EXPECT_EQ(readLE<uint32_t>(Code), kPACIBSPPC);
  EXPECT_FALSE(Function->Dwarf);
}

TEST_F(AArch64LinkedUnwind, LinkerPreservesLegacySigningPCFromEscapedCFI) {
  checkDwarf(false);
}

TEST_F(AArch64LinkedUnwind, LinkerPreservesAbsoluteSigningPCFromEscapedCFI) {
  checkDwarf(true);
}

} // namespace
