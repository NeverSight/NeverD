//===- MachOAuthenticatedNativeTests.cpp - Apple dyld oracle -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Install into a real linked arm64e image and inspect with Apple's dyld_info.
/// Separately qualify native calls and unwinding after a compiler-original
/// execution control succeeds; offline checks never imply runtime support.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/MachO/MachOAuthenticatedFixups.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/MachO/MachOSignaturePolicy.h"
#include "neverd/backend/codegen/MachO/MachOSigner.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/codegen/SourcePatchDependencies.h"
#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
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
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
#define MACHO_AUTH_NATIVE_TEXT(Name, Value) constexpr char Name[] = Value;
#define MACHO_AUTH_NATIVE_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define MACHO_AUTH_NATIVE_ARGS(Name, ...)                                      \
  constexpr llvm::StringLiteral Name[] = {__VA_ARGS__};
#include "MachOAuthenticatedNativeFixtures.def"

void checkAppleChainedRecipes(llvm::StringRef Info,
                              llvm::ArrayRef<uint8_t> Binary,
                              llvm::ArrayRef<MachOAuthenticatedFixup> Fixups) {
  // Apple's symbolicated -fixups printer indexes past its section inventory
  // for sectionless segments. Chain details and imports do not use that path.
  ASSERT_TRUE(Info.contains(kFixupsHeading));
  ASSERT_TRUE(Info.contains(kImportsHeading));
  llvm::SmallVector<llvm::StringRef> Lines;
  Info.split(Lines, '\n');
  uint64_t PreferredAddress = 0;
  forEachMachOLoadCommand(Binary.data(), Binary.size(),
                          [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
                            if (ID == llvm::MachO::LC_SEGMENT_64) {
                              const auto Segment = readMachOSegment(C, true);
                              if (!Segment.FileOff && Segment.FileSize)
                                PreferredAddress = Segment.VMAddr;
                            }
                          });
  const auto Number = [](llvm::StringRef Value, llvm::StringRef Suffix,
                         uint64_t &Result) {
    return Value.consume_back(Suffix) && !Value.getAsInteger(0, Result);
  };
  for (const auto &Fixup : Fixups) {
    uint64_t FileOffset = 0;
    unsigned Owners = 0;
    forEachMachOLoadCommand(
        Binary.data(), Binary.size(),
        [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
          if (ID != llvm::MachO::LC_SEGMENT_64)
            return;
          const auto Segment = readMachOSegment(C, true);
          if (Fixup.Address >= Segment.VMAddr &&
              rangeInBounds(Fixup.Address - Segment.VMAddr, kPointerBytes,
                            Segment.FileSize)) {
            ++Owners;
            FileOffset = Segment.FileOff + Fixup.Address - Segment.VMAddr;
          }
        });
    ASSERT_EQ(Owners, 1u);
    unsigned Matches = 0;
    for (auto Line : Lines) {
      llvm::SmallVector<llvm::StringRef> Fields;
      Line.trim().split(Fields, ' ', -1, false);
      uint64_t Address;
      if (Fields.size() != (Fixup.Authenticated ? kDetailFieldCount
                                                : kPlainDetailFieldCount) ||
          !Number(Fields[kDetailAddress], kColon, Address) ||
          Address != FileOffset)
        continue;
      ++Matches;
      uint64_t Raw, Diversity, AddressDiversity, Target;
      ASSERT_FALSE(Fields[kDetailRaw].getAsInteger(0, Raw));
      EXPECT_EQ(Raw, readLE<uint64_t>(Binary.data() + FileOffset));
      const auto TargetField =
          Fixup.Authenticated ? kDetailTarget : kPlainDetailTarget;
      ASSERT_TRUE(Number(Fields[TargetField], kClosingParenthesis, Target));
      ASSERT_TRUE(Fields[kDetailKind].consume_back(kColon));
      const auto &Recipe = Fixup.Recipe;
      if (Fixup.Authenticated) {
        ASSERT_TRUE(Number(Fields[kDetailDiversity], kComma, Diversity));
        ASSERT_TRUE(
            Number(Fields[kDetailAddressDiversity], kComma, AddressDiversity));
        ASSERT_TRUE(Fields[kDetailKey].consume_back(kComma));
        EXPECT_EQ(Diversity, Recipe.Diversity);
        EXPECT_EQ(AddressDiversity, unsigned(Recipe.AddressDiversity));
        EXPECT_EQ(Fields[kDetailKey], kKeyNames[Recipe.Key]);
      }
      EXPECT_EQ(Fields[kDetailKind],
                Recipe.Symbol.empty()
                    ? (Fixup.Authenticated ? kAuthRebase : kPlainRebase)
                    : (Fixup.Authenticated ? kAuthBind : kPlainBind));
      if (Recipe.Symbol.empty()) {
        EXPECT_EQ(Fields[TargetField - 1], kTargetLabel);
        EXPECT_EQ(Target, Recipe.Target - PreferredAddress);
        continue;
      }
      EXPECT_EQ(Fields[TargetField - 1], kImportLabel);
      unsigned Imports = 0;
      for (auto ImportLine : Lines) {
        llvm::SmallVector<llvm::StringRef> Import;
        ImportLine.trim().split(Import, ' ', -1, false);
        const unsigned Weak = unsigned(Recipe.WeakImport);
        uint64_t Ordinal;
        if (Import.size() != kImportFieldCount + Weak ||
            Import[kImportFrom + Weak] != kFromLabel ||
            Import[kImportOrdinal].getAsInteger(0, Ordinal) ||
            Ordinal != Target)
          continue;
        ++Imports;
        if (Weak)
          EXPECT_EQ(Import[kImportFrom], kWeakImportLabel);
        const auto [Symbol, AddendText] =
            Import[kImportSymbol].split(kAddendSeparator);
        uint64_t Addend = 0;
        if (!AddendText.empty())
          ASSERT_FALSE(AddendText.getAsInteger(0, Addend));
        EXPECT_EQ(Symbol, Recipe.Symbol);
        EXPECT_EQ(Addend, uint64_t(Recipe.Addend));
        ASSERT_TRUE(
            Import[kImportModule + Weak].consume_back(kClosingParenthesis));
        EXPECT_EQ(Import[kImportModule + Weak],
                  llvm::sys::path::filename(Recipe.Module)
                      .split(kLibraryVersionSeparator)
                      .first);
      }
      EXPECT_EQ(Imports, 1u);
    }
    EXPECT_EQ(Matches, 1u);
  }
}

void checkPreservedSignedUnwind(const BinaryImage &Before,
                                const BinaryImage &After) {
  const auto OldCompact = macho_unwind::parseCompactUnwind(Before);
  const auto NewCompact = macho_unwind::parseCompactUnwind(After);
  ASSERT_EQ(OldCompact.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_EQ(NewCompact.ParseStatus, ExceptionParseStatus::Complete);
  const auto CleanupSymbol =
      llvm::find_if(Before.Symbols, [](const auto &Symbol) {
        return Symbol.Name == kCleanupSymbol;
      });
  ASSERT_NE(CleanupSymbol, Before.Symbols.end());
  const auto CleanupVA = CleanupSymbol->Addr;
  const auto CleanupFrame =
      llvm::find_if(OldCompact.Entries, [&](const auto &E) {
        return E.CodeRange.contains(CleanupVA);
      });
  ASSERT_NE(CleanupFrame, OldCompact.Entries.end());
  ASSERT_TRUE(CleanupFrame->HasLSDA);
  ASSERT_NE(CleanupFrame->NativeEncoding & macho_unwind::kPersonalityMask, 0u);
  ASSERT_GE(NewCompact.Entries.size(), OldCompact.Entries.size());
  ASSERT_LE(OldCompact.Personalities.size(), NewCompact.Personalities.size());
  ASSERT_LE(OldCompact.PersonalitySlots.size(),
            NewCompact.PersonalitySlots.size());
  EXPECT_TRUE(std::equal(OldCompact.Personalities.begin(),
                         OldCompact.Personalities.end(),
                         NewCompact.Personalities.begin()));
  EXPECT_TRUE(std::equal(OldCompact.PersonalitySlots.begin(),
                         OldCompact.PersonalitySlots.end(),
                         NewCompact.PersonalitySlots.begin()));
  for (const auto &Old : OldCompact.Entries) {
    const auto Found =
        llvm::find_if(NewCompact.Entries, [&](const auto &Entry) {
          return Entry.CodeRange.Begin == Old.CodeRange.Begin;
        });
    ASSERT_NE(Found, NewCompact.Entries.end());
    const auto &New = *Found;
    EXPECT_EQ(Old.CodeRange.Begin, New.CodeRange.Begin);
    EXPECT_EQ(Old.CodeRange.End, New.CodeRange.End);
    EXPECT_EQ(Old.NativeEncoding, New.NativeEncoding);
    EXPECT_EQ(Old.PersonalityVA, New.PersonalityVA);
    EXPECT_EQ(Old.ReturnAddressSigningOffset, New.ReturnAddressSigningOffset);
    EXPECT_EQ(Old.LSDAVA, New.LSDAVA);
  }
  ::testing::Test::RecordProperty(kCompactFramesProperty,
                                  std::to_string(OldCompact.Entries.size()));
  size_t SignedRows = 0;
  for (const auto &Function : Before.ExceptionMetadata.Functions) {
    if (!Function.Dwarf)
      continue;
    const auto &Old = *Function.Dwarf;
    const auto *New = After.ExceptionMetadata.findFunction(Old.InitialLocation);
    ASSERT_NE(New, nullptr);
    ASSERT_TRUE(New->Dwarf);
    EXPECT_EQ(Old.AddressRange, New->Dwarf->AddressRange);
    const auto *OldCIE = Before.ExceptionMetadata.findCIE(Old.CIESectionOffset);
    const auto *NewCIE =
        After.ExceptionMetadata.findCIE(New->Dwarf->CIESectionOffset);
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
  ::testing::Test::RecordProperty(kSignedRowsProperty,
                                  std::to_string(SignedRows));
}

class MachOArm64eNativeInstallation
    : public ::testing::TestWithParam<std::tuple<const char *, bool, bool>> {};

TEST_P(MachOArm64eNativeInstallation,
       AppleImageKeepsSignedFramesAndDyldRecipes) {
#ifndef __APPLE__
  GTEST_SKIP() << kMissingHost;
#else
  const auto [Optimization, AppendSegments, Threaded] = GetParam();
  RecordProperty(kThreadedProperty, Threaded);
  RecordProperty(kAppendProperty, AppendSegments);
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
  std::string LinkerOption;
  if (Threaded) {
    const auto LinkerPath = Path(kClassicLinkerPath);
    const llvm::StringRef FindArgs[] = {kXcrun, kFindTool, kClassicLinker};
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                        LinkerPath, Log};
    if (llvm::sys::ExecuteAndWait(kXcrun, FindArgs, std::nullopt, Redirects,
                                  kToolTimeout))
      GTEST_SKIP() << kClassicMissing;
    auto Linker = llvm::MemoryBuffer::getFile(LinkerPath);
    ASSERT_TRUE(bool(Linker));
    LinkerOption =
        std::string(kLinkerOption) + (*Linker)->getBuffer().trim().str();
    Args.push_back(LinkerOption);
    Args.push_back(kLegacyDeployment);
    Args.push_back(kThreadedOption);
  } else {
    Args.push_back(kChainedOption);
  }
  Args.push_back(Optimization);
  RecordProperty(kOptimizationProperty, Optimization);
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
  auto Slots = SymbolVA(kSlotSymbol);
  ASSERT_NE(Slots, InvalidVA);
  MachOPatcher Patcher;
  CompiledImage Replacement;
  if (AppendSegments) {
    using Kind = llvm::mc_rewrite::RewriteSectionKind;
    Replacement.Success = true;
    Replacement.InputKind = RewriteInputKind::ReplacementObject;
    Replacement.Format = BinaryFormat::MachO;
    Replacement.TargetArch = Arch::AArch64;
    Replacement.BaseVA =
        Patcher.plannedExecSegmentVA(Before->Raw, Replacement.TargetArch);
    ASSERT_NE(Replacement.BaseVA, InvalidVA);
    Replacement.Bytes.resize(kPageBytes + kSlotCount * kPointerBytes);
    writeLE<uint32_t>(Replacement.Bytes.data(), kReturnInstruction);
    CompiledSection Code, Data;
    Code.Name = kNewCodeSection;
    Code.VA = Replacement.BaseVA;
    Code.Size = kInstructionBytes;
    Code.Kind = Kind::Code;
    Data.Name = kNewDataSegment;
    Data.Offset = kPageBytes;
    Data.VA = Replacement.BaseVA + Data.Offset;
    Data.Size = kSlotCount * kPointerBytes;
    Data.Kind = Kind::WritableData;
    Replacement.Sections = {Code, Data};
    Slots = Data.VA;
  }
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
        else if (AppendSegments)
          Recipe.Target = Replacement.BaseVA;
        Recipe.Key = Key;
        Recipe.AddressDiversity = AddressDiversity;
        Fixups.push_back({Slots + Fixups.size() * kPointerBytes, Recipe});
      }
  }
  std::vector<uint8_t> Bytes((*Input)->getBufferStart(),
                             (*Input)->getBufferEnd());
  auto Error = AppendSegments
                   ? Patcher.appendReplacementImage(Bytes, Replacement, Fixups)
                   : installMachOAuthenticatedFixups(Bytes, Fixups);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  ASSERT_FALSE(Write(InstalledPath, llvm::toStringRef(llvm::ArrayRef(Bytes))));
  Error = macho_signing::adHocResign(InstalledPath,
                                     Signature->Slices.front().Identifier);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = macho_signing::verifyStrict(InstalledPath);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Args.assign({kXcrun});
  if (Threaded)
    Args.append(std::begin(kThreadedInfoArgs), std::end(kThreadedInfoArgs));
  else
    Args.append(std::begin(kInfoArgs), std::end(kInfoArgs));
  Args.push_back(InstalledPath);
  ASSERT_EQ(Run(kXcrun, Args, InfoPath), 0);
  auto Info = llvm::MemoryBuffer::getFile(InfoPath);
  ASSERT_TRUE(bool(Info));
  EXPECT_TRUE((*Info)->getBuffer().contains(Threaded ? kThreadedHeading
                                                     : kFixupsHeading));
  if (!Threaded)
    checkAppleChainedRecipes((*Info)->getBuffer(), Bytes, Fixups);
  else {
    // Apple's legacy decoder drops the low key bit and reports only binds.
    llvm::SmallVector<llvm::StringRef> Lines;
    (*Info)->getBuffer().split(Lines, '\n');
    for (const auto &Fixup : Fixups) {
      if (Fixup.Recipe.Symbol.empty())
        continue;
      unsigned Matches = 0;
      for (auto Line : Lines) {
        llvm::SmallVector<llvm::StringRef> Fields;
        Line.trim().split(Fields, ' ', -1, false);
        if (Fields.size() != kThreadedInfoCount)
          continue;
        uint64_t Address;
        if (Fields[kThreadedInfoAddress].getAsInteger(0, Address) ||
            Address != Fixup.Address)
          continue;
        ++Matches;
        unsigned Diversity;
        ASSERT_FALSE(Fields[kThreadedInfoDiversity].getAsInteger(0, Diversity));
        EXPECT_EQ(Diversity, Fixup.Recipe.Diversity);
        EXPECT_EQ(Fields[kThreadedInfoAddressDiversity],
                  Fixup.Recipe.AddressDiversity ? kTrue : kFalse);
        uint64_t Pointer;
        ASSERT_FALSE(Fields[kThreadedInfoPointer].getAsInteger(0, Pointer));
        EXPECT_EQ((Pointer >> kThreadedPointerKeyShift) &
                      kThreadedPointerKeyMask,
                  Fixup.Recipe.Key);
        if (!(Fixup.Recipe.Key & kThreadedToolMissingKeyBit))
          EXPECT_EQ(Fields[kThreadedInfoKey], kKeyNames[Fixup.Recipe.Key]);
        EXPECT_EQ(Fields[kThreadedInfoSymbol], Fixup.Recipe.Symbol);
      }
      EXPECT_EQ(Matches, 1u);
    }
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
  EXPECT_EQ(macho_unwind::parseCompactUnwind(*Before).Entries.size(),
            macho_unwind::parseCompactUnwind(*After).Entries.size());
  checkPreservedSignedUnwind(*Before, *After);
#endif
}
INSTANTIATE_TEST_SUITE_P(AppleClang, MachOArm64eNativeInstallation,
                         ::testing::Combine(::testing::Values(kO0, kO2),
                                            ::testing::Bool(),
                                            ::testing::Bool()));

class MachOArm64eNativeObjectInstallation
    : public ::testing::TestWithParam<std::tuple<const char *, bool>> {};

void checkNativeObjectInstallation(const char *Optimization, bool Threaded,
                                   bool ExecuteNative,
                                   bool DwarfLanguage = false,
                                   bool FreshPersonality = false) {
#ifndef __APPLE__
  GTEST_SKIP() << kMissingHost;
#else
  ::testing::Test::RecordProperty(kOptimizationProperty, Optimization);
  ::testing::Test::RecordProperty(kThreadedProperty, Threaded);
  llvm::SmallString<kPathCapacity> Directory;
  const auto *Root = std::getenv(kArtifactEnvironment);
  if (Root) {
    ASSERT_FALSE(llvm::sys::fs::create_directories(Root));
    llvm::SmallString<kPathCapacity> Prefix(Root);
    llvm::sys::path::append(Prefix, kObjectArtifactPrefix);
    ASSERT_FALSE(llvm::sys::fs::make_absolute(Prefix));
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
    ::testing::Test::RecordProperty(kArtifactProperty, Directory.str().str());
  } else
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory(kObjectArtifactPrefix, Directory));
  const auto Cleanup = llvm::scope_exit([&] {
    if (!Root)
      EXPECT_FALSE(llvm::sys::fs::remove_directories(Directory));
  });
  const auto Path = [&](llvm::StringRef Name) {
    llvm::SmallString<kPathCapacity> Value(Directory);
    llvm::sys::path::append(Value, Name);
    return Value.str().str();
  };
  const auto Write = [&](llvm::StringRef Filename, llvm::StringRef Bytes) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Filename, Error);
    if (!Error) {
      Out << Bytes;
      Out.close();
      Error = Out.error();
    }
    return Error;
  };
  const auto Source = Path(kSourceFile), Dwarf = Path(kDwarfSourceFile),
             ObjectSource = Path(kObjectSourceFile),
             ObjectPath = Path(kObjectFile), OriginalPath = Path(kOriginalFile),
             InstalledPath = Path(kInstalledFile), Log = Path(kToolLog),
             InfoPath = Path(kDyldInfoFile);
  const auto Run =
      [&](llvm::StringRef Tool, llvm::ArrayRef<llvm::StringRef> Args,
          llvm::StringRef Output = {}) {
        const std::optional<llvm::StringRef> Redirects[] = {
            std::nullopt, Output.empty() ? std::nullopt : std::optional(Output),
            Log};
        std::string Error;
        const int Status = llvm::sys::ExecuteAndWait(
            Tool, Args, std::nullopt, Redirects, kToolTimeout, 0, &Error);
        auto Diagnostic = llvm::MemoryBuffer::getFile(Log);
        EXPECT_EQ(Status, 0)
            << Error
            << (Diagnostic ? (*Diagnostic)->getBuffer().str() : std::string());
        return Status;
      };
  ASSERT_FALSE(Write(Source, kSource));
  ASSERT_FALSE(Write(Dwarf, kDwarfSource));
  ASSERT_FALSE(Write(ObjectSource, kObjectSource));
  llvm::SmallVector<llvm::StringRef> Args{kCompiler};
  const auto ProviderSource = Path(kRuntimeProviderSourceFile),
             RuntimeSource = Path(kRuntimeHarnessFile),
             ControlPath = Path(kRuntimeControlFile),
             DriverPath = Path(kRuntimeDriverFile),
             ProviderPath = Path(kRuntimeProviderFile),
             RuntimeOutput = Path(kRuntimeOutputFile);
  const std::string RpathOption =
      std::string(kRuntimeRpathPrefix) + Directory.str().str();
  if (ExecuteNative) {
    ASSERT_FALSE(Write(ProviderSource, kRuntimeProviderSource));
    ASSERT_FALSE(Write(RuntimeSource, kRuntimeHarness));
    Args.append(std::begin(kRuntimeCompileArgs), std::end(kRuntimeCompileArgs));
    if (DwarfLanguage)
      Args.push_back(kDwarfLanguageOption);
    Args.append({Optimization, kRuntimeControlOption, ObjectSource,
                 ProviderSource, RuntimeSource});
    Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
    Args.push_back(ControlPath);
    ASSERT_EQ(Run(kCompiler, Args), 0);
    Args.assign({kCompiler});
    Args.append(std::begin(kRuntimeCompileArgs), std::end(kRuntimeCompileArgs));
    Args.append(std::begin(kProviderCompileArgs),
                std::end(kProviderCompileArgs));
    Args.append({Optimization, kRuntimeInstallName, ProviderSource});
    Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
    Args.push_back(ProviderPath);
    ASSERT_EQ(Run(kCompiler, Args), 0);
    Args.assign({kCompiler});
    Args.append(std::begin(kRuntimeCompileArgs), std::end(kRuntimeCompileArgs));
    Args.append({Optimization, RpathOption, RuntimeSource});
    Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
    Args.push_back(DriverPath);
    ASSERT_EQ(Run(kCompiler, Args), 0);
    auto Signed = macho_signing::verifyStrict(ControlPath);
    ASSERT_FALSE(bool(Signed)) << llvm::toString(std::move(Signed));
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                        RuntimeOutput, Log};
    std::string Diagnostic;
    const int Status =
        llvm::sys::ExecuteAndWait(ControlPath, {ControlPath}, std::nullopt,
                                  Redirects, kToolTimeout, 0, &Diagnostic);
    ::testing::Test::RecordProperty(kRuntimeControlResult, Status);
    if (Status < 0)
      GTEST_SKIP() << kRuntimeControlUnavailable << Status << '\n'
                   << Diagnostic;
    ASSERT_EQ(Status, 0) << Diagnostic;
    auto Output = llvm::MemoryBuffer::getFile(RuntimeOutput);
    ASSERT_TRUE(bool(Output));
    ASSERT_TRUE((*Output)->getBuffer().contains(kRuntimeMarker));
  }
  Args.assign({kCompiler});
  Args.append(std::begin(kCompileArgs), std::end(kCompileArgs));
  std::string LinkerOption;
  if (Threaded) {
    const auto LinkerFile = Path(kClassicLinkerPath);
    ASSERT_EQ(Run(kXcrun, {kXcrun, kFindTool, kClassicLinker}, LinkerFile), 0);
    auto Linker = llvm::MemoryBuffer::getFile(LinkerFile);
    ASSERT_TRUE(bool(Linker));
    LinkerOption =
        std::string(kLinkerOption) + (*Linker)->getBuffer().trim().str();
    Args.append({LinkerOption, kLegacyDeployment, kThreadedOption});
  } else
    Args.push_back(kChainedOption);
  Args.append({Optimization, Source, Dwarf});
  Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
  Args.push_back(OriginalPath);
  ASSERT_EQ(Run(kCompiler, Args), 0);
  Args.assign({kCompiler});
  Args.append(std::begin(kObjectCompileArgs), std::end(kObjectCompileArgs));
  if (DwarfLanguage)
    Args.push_back(kDwarfLanguageOption);
  Args.append({Optimization, ObjectSource});
  Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
  Args.push_back(ObjectPath);
  ASSERT_EQ(Run(kCompiler, Args), 0);
  Args.assign({kXcrun});
  Args.append(std::begin(kObjectDisassemblyArgs),
              std::end(kObjectDisassemblyArgs));
  Args.push_back(ObjectPath);
  const auto DisassemblyPath = Path(kObjectDisassembly);
  ASSERT_EQ(Run(kXcrun, Args, DisassemblyPath), 0);
  auto Disassembly = llvm::MemoryBuffer::getFile(DisassemblyPath);
  ASSERT_TRUE(bool(Disassembly));
  EXPECT_TRUE((*Disassembly)->getBuffer().contains(kNativeCallA));
  EXPECT_TRUE((*Disassembly)->getBuffer().contains(kNativeCallB));
  EXPECT_TRUE((*Disassembly)->getBuffer().contains(kNativeReturn));
  auto Input = llvm::MemoryBuffer::getFile(OriginalPath);
  auto Object = llvm::MemoryBuffer::getFile(ObjectPath);
  ASSERT_TRUE(bool(Input));
  ASSERT_TRUE(bool(Object));
  std::vector<uint8_t> Bytes((*Input)->getBufferStart(),
                             (*Input)->getBufferEnd());
  auto Loader = neverd::Loader::create(BinaryFormat::MachO);
  auto Before = Loader->loadBuffer((*Input)->getMemBufferRef());
  ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
  auto Signature = macho_signature::inspect(Bytes);
  ASSERT_TRUE(bool(Signature)) << llvm::toString(Signature.takeError());
  std::vector<SourcePatchDependency> Dependencies = {
      {kProviderLibrary, kProviderFunction,
       SourcePatchDependencyKind::Function},
      {kProviderLibrary, kProviderData, SourcePatchDependencyKind::Data}};
  if (FreshPersonality)
    Dependencies.push_back(
        {kCxxLibrary, kCxxPersonality, SourcePatchDependencyKind::Function});
  if (!Threaded && llvm::StringRef(Optimization) == kO0) {
    uint64_t Tail = 0, TextEnd = 0;
    forEachMachOLoadCommand(
        Bytes.data(), Bytes.size(),
        [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
          if (ID != llvm::MachO::LC_SEGMENT_64)
            return;
          const auto *Segment =
              reinterpret_cast<const llvm::MachO::segment_command_64 *>(C);
          if (readMachOName(Segment->segname) != kOriginalTextSegment)
            return;
          TextEnd = Segment->fileoff + Segment->filesize;
          const auto *Sections =
              reinterpret_cast<const llvm::MachO::section_64 *>(Segment + 1);
          for (unsigned I = 0; I < Segment->nsects; ++I)
            if (Sections[I].offset)
              Tail = std::max(Tail, Sections[I].offset + Sections[I].size);
        });
    ASSERT_LT(Tail, TextEnd);
    ASSERT_TRUE(llvm::all_of(llvm::ArrayRef(Bytes).slice(Tail, TextEnd - Tail),
                             [](uint8_t Byte) { return Byte == 0; }));
    const auto RejectReservation = [&](std::vector<uint8_t> Candidate) {
      const auto Snapshot = Candidate;
      auto Grown = reserveMachOSourceUnwind(Candidate, Arch::AArch64, 0,
                                            kObjectFunctionCount);
      EXPECT_FALSE(bool(Grown));
      llvm::consumeError(Grown.takeError());
      EXPECT_EQ(Candidate, Snapshot);
    };
    auto Occupied = Bytes;
    std::fill(Occupied.begin() + Tail, Occupied.begin() + TextEnd,
              kOccupiedMetadataPadding);
    RejectReservation(std::move(Occupied));
    auto Noted = Bytes;
    const auto Header = parseMachOHeader(Noted.data(), Noted.size());
    const auto NoteOffset = Header.HeaderSize + Header.SizeOfCmds;
    ASSERT_LT(NoteOffset + sizeof(llvm::MachO::note_command),
              findFirstMachODataOffset(Noted.data(), Noted.size()));
    auto *Note = reinterpret_cast<llvm::MachO::note_command *>(Noted.data() +
                                                               NoteOffset);
    *Note = {};
    Note->cmd = llvm::MachO::LC_NOTE;
    Note->cmdsize = sizeof(*Note);
    std::copy(std::begin(kPaddingNoteOwner), std::end(kPaddingNoteOwner),
              Note->data_owner);
    Note->offset = Tail;
    Note->size = TextEnd - Tail;
    setMachOHeaderCmds(Noted.data(), true, Header.NCmds + 1,
                       Header.SizeOfCmds + sizeof(*Note));
    RejectReservation(std::move(Noted));
    std::optional<std::pair<uint64_t, uint64_t>> AuthField;
    std::optional<uint64_t> PlainRelocation, ExceptionTable;
    std::optional<uint64_t> PrivateWeakSymbol;
    auto ObjectBytes = llvm::arrayRefFromStringRef((*Object)->getBuffer());
    forEachMachOLoadCommand(
        ObjectBytes.data(), ObjectBytes.size(),
        [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
          if (ID == llvm::MachO::LC_SYMTAB) {
            const auto *Symbols =
                reinterpret_cast<const llvm::MachO::symtab_command *>(C);
            for (unsigned I = 0; I < Symbols->nsyms; ++I) {
              const auto Offset =
                  Symbols->symoff + I * sizeof(llvm::MachO::nlist_64);
              const auto *Symbol =
                  reinterpret_cast<const llvm::MachO::nlist_64 *>(
                      ObjectBytes.data() + Offset);
              if ((Symbol->n_desc & llvm::MachO::N_WEAK_DEF) &&
                  (Symbol->n_type & llvm::MachO::N_PEXT))
                PrivateWeakSymbol = Offset;
            }
          }
          if (ID != llvm::MachO::LC_SEGMENT_64)
            return;
          const auto *Segment =
              reinterpret_cast<const llvm::MachO::segment_command_64 *>(C);
          const auto *Sections =
              reinterpret_cast<const llvm::MachO::section_64 *>(Segment + 1);
          for (unsigned I = 0; I < Segment->nsects; ++I) {
            if (readMachOName(Sections[I].sectname) == kExceptionSection)
              ExceptionTable = Sections[I].offset;
            for (unsigned J = 0; J < Sections[I].nreloc; ++J) {
              const uint64_t Offset =
                  Sections[I].reloff +
                  J * sizeof(llvm::MachO::any_relocation_info) +
                  sizeof(uint32_t);
              const auto Info = readLE<uint32_t>(ObjectBytes.data() + Offset);
              if (readMachOName(Sections[I].segname) == kObjectDataSegment &&
                  (Info >> kObjectRelocTypeShift) ==
                      llvm::MachO::ARM64_RELOC_UNSIGNED)
                PlainRelocation = Offset;
            }
            if (readMachOName(Sections[I].segname) == kObjectConstSegment &&
                Sections[I].nreloc)
              AuthField = {
                  Sections[I].offset +
                      readLE<uint32_t>(ObjectBytes.data() + Sections[I].reloff),
                  Sections[I].reloff};
          }
        });
    ASSERT_TRUE(AuthField);
    ASSERT_TRUE(PlainRelocation);
    ASSERT_TRUE(ExceptionTable);
    const auto Check = [&](auto Change) {
      auto Corrupt = (*Object)->getBuffer().str();
      Change(reinterpret_cast<uint8_t *>(Corrupt.data()));
      auto Candidate = Bytes;
      auto Rejected = appendMachOObjectImage(Candidate, Corrupt, Dependencies);
      EXPECT_FALSE(bool(Rejected));
      llvm::consumeError(Rejected.takeError());
      EXPECT_EQ(Candidate, Bytes);
    };
    Check([&](uint8_t *Data) {
      writeLE<uint64_t>(Data + AuthField->first,
                        readLE<uint64_t>(Data + AuthField->first) |
                            kObjectReservedBit);
    });
    Check([&](uint8_t *Data) {
      writeLE<uint64_t>(Data + AuthField->first,
                        readLE<uint64_t>(Data + AuthField->first) &
                            ~kObjectAuthenticationBit);
    });
    Check([&](uint8_t *Data) {
      auto *Flags = Data + AuthField->second + sizeof(uint32_t);
      writeLE<uint32_t>(Flags, readLE<uint32_t>(Flags) &
                                   ~kObjectExternalRelocationBit);
    });
    Check([&](uint8_t *Data) {
      reinterpret_cast<llvm::MachO::mach_header_64 *>(Data)->cpusubtype =
          llvm::MachO::CPU_SUBTYPE_ARM64_ALL;
    });
    Check([&](uint8_t *Data) {
      reinterpret_cast<llvm::MachO::mach_header_64 *>(Data)->cpusubtype ^=
          kObjectABIVersionBit;
    });
    Check([&](uint8_t *Data) {
      writeLE<uint32_t>(Data + *PlainRelocation,
                        readLE<uint32_t>(Data + *PlainRelocation) |
                            kObjectPCRelativeBit);
    });
    Check([&](uint8_t *Data) {
      Data[*ExceptionTable] = kUnsupportedLSDAEncoding;
    });
    ASSERT_TRUE(PrivateWeakSymbol);
    Check([&](uint8_t *Data) {
      reinterpret_cast<llvm::MachO::nlist_64 *>(Data + *PrivateWeakSymbol)
          ->n_type &= ~llvm::MachO::N_PEXT;
    });
  }
  auto Image =
      appendMachOObjectImage(Bytes, (*Object)->getBuffer(), Dependencies);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->Success);
  EXPECT_EQ(Image->InputKind, RewriteInputKind::ReplacementObject);
  auto Recipes = collectMachOAuthenticatedFixups(Bytes, *Image, Dependencies);
  ASSERT_TRUE(bool(Recipes)) << llvm::toString(Recipes.takeError());
  EXPECT_EQ(
      llvm::count_if(*Recipes, [](const auto &R) { return R.Authenticated; }),
      kKeyCount + unsigned(FreshPersonality));
  EXPECT_EQ(Recipes->size(),
            kKeyCount + kOrdinaryPointerCount + unsigned(FreshPersonality));
  const char *Slots[] = {kObjectLocalCode, kObjectExternalCode,
                         kObjectLocalData, kObjectExternalData};
  for (unsigned Key = 0; Key != kKeyCount; ++Key) {
    ASSERT_TRUE(Image->SymbolAddrs.contains(Slots[Key]));
    const auto Address = Image->SymbolAddrs.at(Slots[Key]);
    const auto Fixup = llvm::find_if(
        *Recipes, [&](const auto &R) { return R.Address == Address; });
    ASSERT_NE(Fixup, Recipes->end());
    EXPECT_EQ(Fixup->Recipe.Key, Key);
    EXPECT_EQ(Fixup->Recipe.Diversity, kObjectDiversity);
    EXPECT_TRUE(Fixup->Recipe.AddressDiversity);
    if (Key == 0)
      EXPECT_EQ(Fixup->Recipe.Target, Image->SymbolAddrs.at(kObjectFunction));
    else if (Key == 2)
      EXPECT_EQ(Fixup->Recipe.Target,
                Image->SymbolAddrs.at(kObjectData) + kObjectDataAddend);
    else {
      EXPECT_EQ(Fixup->Recipe.Module, kProviderLibrary);
      EXPECT_EQ(Fixup->Recipe.Symbol,
                Key == 1 ? kProviderNativeFunction : kProviderNativeData);
      EXPECT_EQ(Fixup->Recipe.Addend, Key == 1 ? 0 : kObjectDataAddend);
    }
  }
  for (const auto *Name : {kPlainLocal, kPlainExternal}) {
    const auto Address = Image->SymbolAddrs.at(Name);
    auto Fixup = llvm::find_if(
        *Recipes, [&](const auto &R) { return R.Address == Address; });
    ASSERT_NE(Fixup, Recipes->end());
    EXPECT_FALSE(Fixup->Authenticated);
    if (llvm::StringRef(Name) == kPlainLocal)
      EXPECT_EQ(Fixup->Recipe.Target,
                Image->SymbolAddrs.at(kObjectData) + kObjectDataAddend);
    else {
      EXPECT_EQ(Fixup->Recipe.Symbol, kProviderNativeData);
      EXPECT_EQ(Fixup->Recipe.Addend, kObjectDataAddend);
    }
  }
  unsigned ReadOnly = 0, Mutable = 0;
  forEachMachOLoadCommand(
      Bytes.data(), Bytes.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
        if (ID != llvm::MachO::LC_SEGMENT_64)
          return;
        const auto *S =
            reinterpret_cast<const llvm::MachO::segment_command_64 *>(C);
        const auto Name = readMachOName(S->segname);
        if (Name == kNewConstSegment) {
          ++ReadOnly;
          EXPECT_EQ(S->flags, kReadOnlySegmentFlag);
          EXPECT_EQ(S->initprot,
                    llvm::MachO::VM_PROT_READ | llvm::MachO::VM_PROT_WRITE);
          EXPECT_EQ(S->vmaddr % kPageBytes, 0u);
          EXPECT_EQ(S->vmsize % kPageBytes, 0u);
        }
        if (Name == kNewDataSegment) {
          ++Mutable;
          EXPECT_EQ(S->flags, 0u);
        }
      });
  EXPECT_EQ(ReadOnly, 1u);
  EXPECT_EQ(Mutable, 1u);
  ASSERT_FALSE(Write(InstalledPath, llvm::toStringRef(llvm::ArrayRef(Bytes))));
  auto Error = macho_signing::adHocResign(InstalledPath,
                                          Signature->Slices.front().Identifier);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = macho_signing::verifyStrict(InstalledPath);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Args.assign({kXcrun});
  if (Threaded)
    Args.append(std::begin(kThreadedInfoArgs), std::end(kThreadedInfoArgs));
  else
    Args.append(std::begin(kInfoArgs), std::end(kInfoArgs));
  Args.push_back(InstalledPath);
  ASSERT_EQ(Run(kXcrun, Args, InfoPath), 0);
  auto Info = llvm::MemoryBuffer::getFile(InfoPath);
  ASSERT_TRUE(bool(Info));
  EXPECT_TRUE((*Info)->getBuffer().contains(kProviderNativeFunction));
  EXPECT_TRUE((*Info)->getBuffer().contains(kProviderNativeData));
  if (!Threaded)
    checkAppleChainedRecipes((*Info)->getBuffer(), Bytes, *Recipes);
  Input = llvm::MemoryBuffer::getFile(InstalledPath);
  ASSERT_TRUE(bool(Input));
  auto Final = llvm::arrayRefFromStringRef((*Input)->getBuffer());
  Error = verifyMachOAuthenticatedFixups(Final, *Recipes);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  auto MissingProtection = std::vector<uint8_t>(Final.begin(), Final.end());
  forEachMachOLoadCommand(
      MissingProtection.data(), MissingProtection.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
        if (ID == llvm::MachO::LC_SEGMENT_64) {
          const auto *S =
              reinterpret_cast<const llvm::MachO::segment_command_64 *>(C);
          if (readMachOName(S->segname) == kNewConstSegment)
            writeLE<uint32_t>(
                MissingProtection.data() + (C - MissingProtection.data()) +
                    offsetof(llvm::MachO::segment_command_64, flags),
                0);
        }
      });
  Error = verifyMachOAuthenticatedFixups(MissingProtection, *Recipes);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  auto Parsed = Loader->loadBuffer((*Input)->getMemBufferRef());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  checkPreservedSignedUnwind(*Before, *Parsed);
  for (const auto &Recipe : *Recipes) {
    if (Recipe.Recipe.Symbol.empty() || Recipe.Recipe.Addend)
      continue;
    EXPECT_EQ(resolveRoutineName(*Parsed, 0, Recipe.Address),
              Recipe.Recipe.Symbol);
    EXPECT_FALSE(Parsed->ImportPtrSlots.contains(Recipe.Address));
    if (Recipe.Authenticated)
      EXPECT_FALSE(Parsed->ImportStorageSlots.contains(Recipe.Address));
  }
  for (const auto &Section : Image->Sections) {
    if (Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code)
      continue;
    const auto *Installed = Parsed->readVA(Section.VA, Section.Size);
    ASSERT_NE(Installed, nullptr);
    EXPECT_TRUE(std::equal(Image->Bytes.begin() + Section.Offset,
                           Image->Bytes.begin() + Section.Offset + Section.Size,
                           Installed));
  }
  const auto Native = macho_unwind::parseCompactUnwind(*Parsed);
  ASSERT_EQ(Native.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_TRUE(llvm::any_of(Native.Entries, [&](const auto &Entry) {
    return Entry.CodeRange.Begin == Image->SymbolAddrs.at(kObjectFunction) &&
           Entry.SemanticStatus == CompactUnwindSemanticStatus::Complete;
  }));
  EXPECT_TRUE(llvm::any_of(Native.Entries, [&](const auto &Entry) {
    return Entry.CodeRange.Begin == Image->SymbolAddrs.at(kObjectDispatch) &&
           Entry.SemanticStatus == CompactUnwindSemanticStatus::Complete;
  }));
  std::string DwarfInfo;
  if (DwarfLanguage) {
    const auto DwarfInfoPath = Path(kDwarfInfoFile);
    Args.assign({kXcrun});
    Args.append(std::begin(kDwarfInfoArgs), std::end(kDwarfInfoArgs));
    Args.push_back(InstalledPath);
    ASSERT_EQ(Run(kXcrun, Args, DwarfInfoPath), 0);
    auto Dump = llvm::MemoryBuffer::getFile(DwarfInfoPath);
    ASSERT_TRUE(bool(Dump));
    DwarfInfo = (*Dump)->getBuffer().str();
    EXPECT_TRUE(
        llvm::StringRef(DwarfInfo).contains(kDwarfLanguageAugmentation));
  }
  for (const auto *Name : {kObjectCatch, kObjectCleanup}) {
    const auto *Function =
        Parsed->ExceptionMetadata.findFunction(Image->SymbolAddrs.at(Name));
    ASSERT_NE(Function, nullptr);
    EXPECT_EQ(Function->ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(Function->Personality, ExceptionPersonality::GxxPersonalityV0);
    ASSERT_TRUE(Function->Itanium);
    EXPECT_FALSE(Function->Itanium->CallSites.empty());
    ASSERT_TRUE(Function->Compact);
    EXPECT_TRUE(Function->Compact->HasLSDA);
    EXPECT_EQ(Function->Dwarf.has_value(), DwarfLanguage);
    if (Function->Dwarf) {
      const auto *CIE =
          Parsed->ExceptionMetadata.findCIE(Function->Dwarf->CIESectionOffset);
      ASSERT_NE(CIE, nullptr);
      EXPECT_TRUE(CIE->UsesArm64eReturnAddressABI);
      EXPECT_NE(CIE->PersonalitySlotVA, 0u);
      EXPECT_EQ(Function->Dwarf->LSDAVA, Function->HandlerDataVA);
      const auto Address = [](uint64_t VA, unsigned Width) {
        auto Text = llvm::utohexstr(VA, true);
        return std::string(Width > Text.size() ? Width - Text.size() : 0, '0') +
               Text;
      };
      EXPECT_TRUE(llvm::StringRef(DwarfInfo).contains(
          kDwarfPCPrefix + Address(Function->CodeRange.Begin, kDwarfPCDigits) +
          kDwarfRangeSeparator +
          Address(Function->CodeRange.End, kDwarfPCDigits)));
      EXPECT_TRUE(llvm::StringRef(DwarfInfo).contains(
          kDwarfLSDAPrefix +
          Address(Function->HandlerDataVA, kDwarfAddressDigits)));
    }
    if (llvm::StringRef(Name) == kObjectCatch)
      EXPECT_TRUE(llvm::any_of(Function->Itanium->TypeTable, [](const auto &T) {
        return T.TypeName == kTypeInfo;
      }));
  }
  if (ExecuteNative) {
    auto Anchor = llvm::find_if(Parsed->Symbols, [&](const auto &Symbol) {
      return Symbol.Name == kSlotSymbol;
    });
    ASSERT_NE(Anchor, Parsed->Symbols.end());
    const std::string Offsets[] = {
        std::to_string(Image->SymbolAddrs.at(kObjectDispatch) - Anchor->Addr),
        std::to_string(Image->SymbolAddrs.at(kObjectLocalData) - Anchor->Addr),
        std::to_string(Image->SymbolAddrs.at(kObjectExternalData) -
                       Anchor->Addr),
        std::to_string(Image->SymbolAddrs.at(kObjectLocalCode) - Anchor->Addr),
        std::to_string(Image->SymbolAddrs.at(kObjectCatch) - Anchor->Addr),
        std::to_string(Image->SymbolAddrs.at(kObjectCleanup) - Anchor->Addr),
        std::to_string(Image->SymbolAddrs.at(kObjectCleanups) - Anchor->Addr)};
    Args.assign({DriverPath, InstalledPath});
    Args.append(std::begin(Offsets), std::end(Offsets));
    ASSERT_EQ(Run(DriverPath, Args, RuntimeOutput), 0);
    auto Output = llvm::MemoryBuffer::getFile(RuntimeOutput);
    ASSERT_TRUE(bool(Output));
    EXPECT_TRUE((*Output)->getBuffer().contains(kRuntimeMarker));
  }
#endif
}

TEST_P(MachOArm64eNativeObjectInstallation,
       NativeRelocationsAdmitProvidersAndRelro) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, false);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       AuthenticatedCallsAndSignedUnwindExecute) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, true);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       NativeDwarfLanguageAndOrdinaryPointers) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, false, true);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       NativeDwarfLanguageCallsAndCleanup) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, true, true);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       InstallsNewAuthenticatedPersonalitySlot) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, false, true, true);
}

INSTANTIATE_TEST_SUITE_P(AppleClang, MachOArm64eNativeObjectInstallation,
                         ::testing::Combine(::testing::Values(kO0, kO2),
                                            ::testing::Bool()));

TEST(MachONativeRelro, DyldProtectsConstantsAndKeepsMutablePagesWritable) {
#if !defined(__APPLE__) || !defined(__aarch64__)
  GTEST_SKIP() << kRelroNativeHost;
#else
  llvm::SmallString<kPathCapacity> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory(kObjectArtifactPrefix, Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { EXPECT_FALSE(llvm::sys::fs::remove_directories(Directory)); });
  const auto Path = [&](llvm::StringRef Name) {
    llvm::SmallString<kPathCapacity> Value(Directory);
    llvm::sys::path::append(Value, Name);
    return Value.str().str();
  };
  const auto Source = Path(kSourceFile), Runtime = Path(kRuntimeHarnessFile),
             Original = Path(kOriginalFile), Installed = Path(kInstalledFile),
             Driver = Path(kRuntimeDriverFile),
             Output = Path(kRuntimeOutputFile);
  const auto Write = [&](llvm::StringRef Name, llvm::StringRef Contents) {
    std::error_code Error;
    llvm::raw_fd_ostream Stream(Name, Error);
    if (!Error) {
      Stream << Contents;
      Stream.close();
      Error = Stream.error();
    }
    return Error;
  };
  ASSERT_FALSE(Write(Source, kRelroOriginalSource));
  ASSERT_FALSE(Write(Runtime, kRelroRuntimeSource));
  llvm::SmallVector<llvm::StringRef> Args{kCompiler};
  Args.append(std::begin(kRelroCompileArgs), std::end(kRelroCompileArgs));
  Args.append(std::begin(kProviderCompileArgs), std::end(kProviderCompileArgs));
  Args.append({kRelroHeaderOption, Source});
  Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
  Args.push_back(Original);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, Args, std::nullopt, {},
                                      kToolTimeout),
            0);
  Args.assign({kCompiler});
  Args.append(std::begin(kRelroCompileArgs), std::end(kRelroCompileArgs));
  Args.push_back(Runtime);
  Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
  Args.push_back(Driver);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, Args, std::nullopt, {},
                                      kToolTimeout),
            0);
  auto Input = llvm::MemoryBuffer::getFile(Original);
  ASSERT_TRUE(bool(Input));
  std::vector<uint8_t> Bytes((*Input)->getBufferStart(),
                             (*Input)->getBufferEnd());
  auto Signature = macho_signature::inspect(Bytes);
  ASSERT_TRUE(bool(Signature)) << llvm::toString(Signature.takeError());
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Before = Reader->loadBuffer((*Input)->getMemBufferRef());
  ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
  const auto Anchor = llvm::find_if(Before->Symbols, [](const auto &Symbol) {
    return Symbol.Name == kSlotSymbol;
  });
  ASSERT_NE(Anchor, Before->Symbols.end());
  MachOPatcher Patcher;
  CompiledImage Image;
  Image.Success = true;
  Image.Format = BinaryFormat::MachO;
  Image.TargetArch = Arch::AArch64;
  Image.InputKind = RewriteInputKind::ReplacementObject;
  Image.BaseVA = Patcher.plannedExecSegmentVA(Bytes, Image.TargetArch);
  ASSERT_NE(Image.BaseVA, 0u);
  Image.Bytes.resize(2 * kPageBytes + kPointerBytes);
  writeLE<uint32_t>(Image.Bytes.data(), kReturnInstruction);
  writeLE<uint64_t>(Image.Bytes.data() + kPageBytes, kRelroRuntimeValue);
  CompiledSection Code, Constant, Mutable;
  Code.Name = kNewCodeSection;
  Code.VA = Image.BaseVA;
  Code.Size = kInstructionBytes;
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  Constant.Name = kNewConstSegment;
  Constant.Offset = kPageBytes;
  Constant.VA = Image.BaseVA + Constant.Offset;
  Constant.Size = kPointerBytes;
  Constant.Kind = llvm::mc_rewrite::RewriteSectionKind::WritableData;
  Constant.ReadOnlyAfterFixups = true;
  Mutable = Constant;
  Mutable.Name = kNewDataSegment;
  Mutable.Offset += kPageBytes;
  Mutable.VA += kPageBytes;
  Mutable.ReadOnlyAfterFixups = false;
  Image.Sections = {Code, Constant, Mutable};
  auto Error = Patcher.appendReplacementImage(Bytes, Image);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  ASSERT_FALSE(Write(Installed, llvm::toStringRef(llvm::ArrayRef(Bytes))));
  Error = macho_signing::adHocResign(Installed,
                                     Signature->Slices.front().Identifier);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = macho_signing::verifyStrict(Installed);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  const auto ConstantOffset = std::to_string(Constant.VA - Anchor->Addr);
  const auto MutableOffset = std::to_string(Mutable.VA - Anchor->Addr);
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      std::nullopt};
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                Driver, {Driver, Installed, ConstantOffset, MutableOffset},
                std::nullopt, Redirects, kToolTimeout),
            0);
  auto Result = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Result));
  EXPECT_TRUE((*Result)->getBuffer().contains(kRelroRuntimeMarker));
#endif
}
} // namespace
