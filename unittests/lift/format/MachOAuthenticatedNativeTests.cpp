//===- MachOAuthenticatedNativeTests.cpp - Apple dyld recipe oracle
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Install into a real linked arm64e image and inspect with Apple's dyld_info.
/// This is an offline check; it does not execute dyld or the signed unwinder.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/MachO/MachOAuthenticatedFixups.h"
#include "neverd/backend/codegen/MachO/MachOSignaturePolicy.h"
#include "neverd/backend/codegen/MachO/MachOSigner.h"
#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/CompactUnwind.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
#define MACHO_AUTH_NATIVE_TEXT(Name, Value) constexpr char Name[] = Value;
#define MACHO_AUTH_NATIVE_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define MACHO_AUTH_NATIVE_ARGS(Name, ...)                                      \
  constexpr llvm::StringLiteral Name[] = {__VA_ARGS__};
#include "MachOAuthenticatedNativeFixtures.def"

class MachOArm64eNativeInstallation
    : public ::testing::TestWithParam<const char *> {};

TEST_P(MachOArm64eNativeInstallation,
       AppleImageKeepsSignedFramesAndDyldRecipes) {
#ifndef __APPLE__
  GTEST_SKIP() << kMissingHost;
#else
  llvm::SmallString<kPathCapacity> Directory;
  const auto *Root = std::getenv(kArtifactEnvironment);
  if (Root) {
    ASSERT_FALSE(llvm::sys::fs::create_directories(Root));
    llvm::SmallString<kPathCapacity> Prefix(Root);
    llvm::sys::path::append(Prefix, kTemporaryPrefix);
    ASSERT_FALSE(llvm::sys::fs::make_absolute(Prefix));
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
    RecordProperty(kArtifactProperty, Directory.str().str());
  } else {
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory(kTemporaryPrefix, Directory));
  }
  const auto Cleanup = llvm::scope_exit([&] {
    if (!Root)
      EXPECT_FALSE(llvm::sys::fs::remove_directories(Directory));
  });
  const auto Path = [&](llvm::StringRef Name) {
    llvm::SmallString<kPathCapacity> Result(Directory);
    llvm::sys::path::append(Result, Name);
    return Result.str().str();
  };
  const auto Source = Path(kSourceFile), DwarfSource = Path(kDwarfSourceFile),
             OriginalPath = Path(kOriginalFile),
             InstalledPath = Path(kInstalledFile), Log = Path(kToolLog),
             InfoPath = Path(kDyldInfoFile);
  const auto Write = [&](llvm::StringRef Filename, llvm::StringRef Bytes) {
    std::error_code Error;
    llvm::raw_fd_ostream Output(Filename, Error);
    if (!Error) {
      Output << Bytes;
      Output.close();
      Error = Output.error();
    }
    return Error;
  };
  ASSERT_FALSE(Write(Source, kSource));
  ASSERT_FALSE(Write(DwarfSource, kDwarfSource));
  const auto Run = [&](llvm::StringRef Tool,
                       llvm::ArrayRef<llvm::StringRef> Args,
                       llvm::StringRef Output = {}) {
    std::string Error;
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, Output.empty() ? std::nullopt : std::optional(Output),
        Log};
    const auto Status = llvm::sys::ExecuteAndWait(
        Tool, Args, std::nullopt, Redirects, kToolTimeout, 0, &Error);
    auto Diagnostics = llvm::MemoryBuffer::getFile(Log);
    EXPECT_EQ(Status, 0) << Error
                         << (Diagnostics ? (*Diagnostics)->getBuffer().str()
                                         : std::string());
    return Status;
  };
  llvm::SmallVector<llvm::StringRef> Args{kCompiler};
  Args.append(std::begin(kCompileArgs), std::end(kCompileArgs));
  Args.push_back(GetParam());
  RecordProperty(kOptimizationProperty, GetParam());
  Args.push_back(Source);
  Args.push_back(DwarfSource);
  Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
  Args.push_back(OriginalPath);
  ASSERT_EQ(Run(kCompiler, Args), 0);
  auto Input = llvm::MemoryBuffer::getFile(OriginalPath);
  ASSERT_TRUE(bool(Input));
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Before = Reader->loadBuffer((*Input)->getMemBufferRef());
  ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
  ASSERT_TRUE(Before->isArm64e());
  auto Signature = macho_signature::inspect(Before->Raw);
  ASSERT_TRUE(bool(Signature)) << llvm::toString(Signature.takeError());
  ASSERT_TRUE(macho_signature::canTransactionallyAdHocResign(*Signature));
  const auto SymbolVA = [&](llvm::StringRef Name) -> va_t {
    for (const auto &Symbol : Before->Symbols)
      if (Symbol.Name == Name)
        return Symbol.Addr;
    return InvalidVA;
  };
  const auto Slots = SymbolVA(kSlotSymbol);
  ASSERT_NE(Slots, InvalidVA);
  std::vector<MachOAuthenticatedFixup> Fixups;
  for (const auto *Symbol : {kRebaseSymbol, kBindSymbol}) {
    const auto Found =
        Before->MachOAuthenticatedPointers.find(SymbolVA(Symbol));
    ASSERT_NE(Found, Before->MachOAuthenticatedPointers.end());
    for (uint8_t Key = 0; Key < kKeyCount; ++Key)
      for (bool AddressDiversity : {false, true}) {
        auto Recipe = Found->second;
        Recipe.EncodedValue.reset();
        if (!Recipe.Symbol.empty())
          Recipe.Symbol = kInstalledImportSymbol;
        Recipe.Key = Key;
        Recipe.AddressDiversity = AddressDiversity;
        Fixups.push_back({Slots + Fixups.size() * kPointerBytes, Recipe});
      }
  }
  std::vector<uint8_t> Bytes((*Input)->getBufferStart(),
                             (*Input)->getBufferEnd());
  auto Error = installMachOAuthenticatedFixups(Bytes, Fixups);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  ASSERT_FALSE(Write(InstalledPath, llvm::toStringRef(llvm::ArrayRef(Bytes))));
  Error = macho_signing::adHocResign(InstalledPath,
                                     Signature->Slices.front().Identifier);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = macho_signing::verifyStrict(InstalledPath);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Args.assign({kXcrun});
  Args.append(std::begin(kInfoArgs), std::end(kInfoArgs));
  Args.push_back(InstalledPath);
  ASSERT_EQ(Run(kXcrun, Args, InfoPath), 0);
  auto Info = llvm::MemoryBuffer::getFile(InfoPath);
  ASSERT_TRUE(bool(Info));
  EXPECT_TRUE((*Info)->getBuffer().contains(kFixupsHeading));
  // Check Apple's independently decoded target and complete signing schema,
  // not just successful parsing or a raw word printed in another column.
  llvm::SmallVector<llvm::StringRef> Lines;
  (*Info)->getBuffer().split(Lines, '\n');
  for (const auto &Fixup : Fixups) {
    unsigned Matches = 0;
    for (auto Line : Lines) {
      llvm::SmallVector<llvm::StringRef> Fields;
      Line.trim().split(Fields, ' ', -1, false);
      if (Fields.size() != kInfoFieldCount ||
          Fields[kInfoSegment] != kDataSegment ||
          Fields[kInfoSection] != kDataSection)
        continue;
      uint64_t Address;
      if (Fields[kInfoAddress].getAsInteger(0, Address) ||
          Address != Fixup.Address)
        continue;
      ++Matches;
      const auto &Recipe = Fixup.Recipe;
      EXPECT_EQ(Fields[kInfoKind],
                Recipe.Symbol.empty() ? kAuthRebase : kAuthBind);
      if (Recipe.Symbol.empty()) {
        uint64_t Target;
        ASSERT_FALSE(Fields[kInfoTarget].getAsInteger(0, Target));
        EXPECT_EQ(Target, Recipe.Target);
      } else {
        EXPECT_TRUE(Fields[kInfoTarget].ends_with(
            std::string(kLibrarySeparator) + Recipe.Symbol));
      }
      ASSERT_TRUE(Fields[kInfoDiversity].consume_front(kDiversityPrefix));
      unsigned Diversity;
      ASSERT_FALSE(Fields[kInfoDiversity].getAsInteger(0, Diversity));
      EXPECT_EQ(Diversity, Recipe.Diversity);
      ASSERT_TRUE(Fields[kInfoAddressDiversity].consume_front(kAddressPrefix));
      unsigned AddressDiversity;
      ASSERT_FALSE(
          Fields[kInfoAddressDiversity].getAsInteger(0, AddressDiversity));
      EXPECT_EQ(AddressDiversity, unsigned(Recipe.AddressDiversity));
      ASSERT_TRUE(Fields[kInfoKey].consume_front(kKeyPrefix));
      ASSERT_TRUE(Fields[kInfoKey].consume_back(kClosingParenthesis));
      EXPECT_EQ(Fields[kInfoKey], kKeyNames[Recipe.Key]);
    }
    EXPECT_EQ(Matches, 1u);
  }
  auto Installed = llvm::MemoryBuffer::getFile(InstalledPath);
  ASSERT_TRUE(bool(Installed));
  auto NewSignature = macho_signature::inspect(
      llvm::arrayRefFromStringRef((*Installed)->getBuffer()));
  ASSERT_TRUE(bool(NewSignature)) << llvm::toString(NewSignature.takeError());
  Error = macho_signature::validateTransactionallyAdHocResigned(*Signature,
                                                                *NewSignature);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = verifyMachOAuthenticatedFixups(
      llvm::arrayRefFromStringRef((*Installed)->getBuffer()), Fixups);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  auto After = Reader->loadBuffer((*Installed)->getMemBufferRef());
  ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
  const auto OldCompact = macho_unwind::parseCompactUnwind(*Before);
  const auto NewCompact = macho_unwind::parseCompactUnwind(*After);
  ASSERT_EQ(OldCompact.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_EQ(NewCompact.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_FALSE(OldCompact.Entries.empty());
  ASSERT_EQ(OldCompact.Entries.size(), NewCompact.Entries.size());
  EXPECT_EQ(OldCompact.Personalities, NewCompact.Personalities);
  EXPECT_EQ(OldCompact.PersonalitySlots, NewCompact.PersonalitySlots);
  for (size_t I = 0; I < OldCompact.Entries.size(); ++I) {
    const auto &Old = OldCompact.Entries[I], &New = NewCompact.Entries[I];
    EXPECT_EQ(Old.CodeRange.Begin, New.CodeRange.Begin);
    EXPECT_EQ(Old.CodeRange.End, New.CodeRange.End);
    EXPECT_EQ(Old.NativeEncoding, New.NativeEncoding);
    EXPECT_EQ(Old.PersonalityVA, New.PersonalityVA);
    EXPECT_EQ(Old.ReturnAddressSigningOffset, New.ReturnAddressSigningOffset);
    EXPECT_EQ(Old.LSDAVA, New.LSDAVA);
  }
  RecordProperty(kCompactFramesProperty,
                 std::to_string(OldCompact.Entries.size()));
  size_t SignedRows = 0;
  for (const auto &Function : Before->ExceptionMetadata.Functions) {
    if (!Function.Dwarf)
      continue;
    const auto &Old = *Function.Dwarf;
    const auto *New =
        After->ExceptionMetadata.findFunction(Old.InitialLocation);
    ASSERT_NE(New, nullptr);
    ASSERT_TRUE(New->Dwarf);
    EXPECT_EQ(Old.AddressRange, New->Dwarf->AddressRange);
    const auto *OldCIE =
        Before->ExceptionMetadata.findCIE(Old.CIESectionOffset);
    const auto *NewCIE =
        After->ExceptionMetadata.findCIE(New->Dwarf->CIESectionOffset);
    ASSERT_NE(OldCIE, nullptr);
    ASSERT_NE(NewCIE, nullptr);
    EXPECT_EQ(Old.LSDAVA, New->Dwarf->LSDAVA);
    EXPECT_EQ(OldCIE->PersonalitySlotVA, NewCIE->PersonalitySlotVA);
    for (uint64_t Offset = 0; Offset < Old.AddressRange;
         Offset += kInstructionBytes) {
      const auto A = dwarf_eh::evaluateAArch64ReturnAddressState(
          Arch::AArch64, *OldCIE, Old, Old.InitialLocation + Offset);
      const auto B = dwarf_eh::evaluateAArch64ReturnAddressState(
          Arch::AArch64, *NewCIE, *New->Dwarf, Old.InitialLocation + Offset);
      ASSERT_TRUE(A.State);
      ASSERT_TRUE(B.State);
      EXPECT_EQ(*A.State, *B.State);
      if (A.State->Kind != dwarf_eh::AArch64RASigningKind::Unsigned) {
        EXPECT_EQ(A.State->Key, dwarf_eh::AArch64RAKey::B);
        ++SignedRows;
      }
    }
  }
  EXPECT_GT(SignedRows, 0u);
  RecordProperty(kSignedRowsProperty, std::to_string(SignedRows));
#endif
}
INSTANTIATE_TEST_SUITE_P(AppleClang, MachOArm64eNativeInstallation,
                         ::testing::Values(kO0, kO2));
} // namespace
