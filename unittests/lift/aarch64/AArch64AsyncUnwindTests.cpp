//===- AArch64AsyncUnwindTests.cpp - Interrupted native return signing ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/AArch64UnwindRecovery.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/ReturnAddressSigning.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugFrame.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
using namespace neverd;
#define ASYNC_FRAME_TEXT(Name, Text) constexpr char Name[] = Text;
#define ASYNC_FRAME_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64AsyncUnwindFixtures.def"

#if defined(__APPLE__) && defined(__aarch64__)
struct StackRow {
  uint64_t Offset;
  int32_t Depth;
  bool SavedLR;
  bool SavedX19;
  bool operator==(const StackRow &) const = default;
};

// Decode the CFA and return-register locations independently from the signed
// column. Only changes of these two locations matter to this fixture.
void readStackRows(const BinaryImage &Image, va_t Entry,
                   std::vector<StackRow> &Rows) {
  const auto Data = llvm::StringRef(
      reinterpret_cast<const char *>(Image.Raw.data()), Image.Raw.size());
  auto Object = llvm::object::ObjectFile::createObjectFile(
      llvm::MemoryBufferRef(Data, {}));
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  auto Debug = llvm::DWARFContext::create(
      **Object, llvm::DWARFContext::ProcessDebugRelocations::Ignore);
  auto Frames = Debug->getEHFrame();
  ASSERT_TRUE(bool(Frames)) << llvm::toString(Frames.takeError());
  using Location = llvm::dwarf::UnwindLocation;
  for (const auto &Frame : (*Frames)->entries()) {
    const auto *FDE = llvm::dyn_cast<llvm::dwarf::FDE>(&Frame);
    if (!FDE || FDE->getInitialLocation() != Entry)
      continue;
    ASSERT_TRUE(Rows.empty());
    auto Table = llvm::dwarf::createUnwindTable(FDE);
    ASSERT_TRUE(bool(Table)) << llvm::toString(Table.takeError());
    for (const auto &Row : *Table) {
      const auto &CFA = Row.getCFAValue();
      ASSERT_EQ(
          CFA, Location::createIsRegisterPlusOffset(kDwarfSP, CFA.getOffset()));
      const auto LR = Row.getRegisterLocations().getRegisterLocation(kDwarfLR);
      const bool Saved = LR && LR->getLocation() != Location::Same;
      if (Saved)
        ASSERT_EQ(*LR,
                  Location::createAtCFAPlusOffset(-int32_t(sizeof(uint64_t))));
      const auto X19 = Row.getRegisterLocations().getRegisterLocation(
          NDArm64eFirstPreservedGPR);
      const bool SavedX19 = X19 && X19->getLocation() != Location::Same;
      if (SavedX19)
        ASSERT_EQ(*X19, Location::createAtCFAPlusOffset(-int32_t(kFrameSize)));
      ASSERT_TRUE(Row.hasAddress());
      ASSERT_GE(Row.getAddress(), Entry);
      if (Rows.empty() || Rows.back().Depth != CFA.getOffset() ||
          Rows.back().SavedLR != Saved || Rows.back().SavedX19 != SavedX19)
        Rows.push_back(
            {Row.getAddress() - Entry, CFA.getOffset(), Saved, SavedX19});
    }
  }
  ASSERT_FALSE(Rows.empty());
  ASSERT_EQ(Rows.front().Offset, 0u);
}

void checkFrame(const BinaryImage &Image, va_t Entry,
                const std::filesystem::path &Samples,
                llvm::ArrayRef<StackRow> Rows,
                llvm::ArrayRef<AArch64RecoveredUnwindRow> Recovered = {}) {
  const auto *Frame = Image.ExceptionMetadata.findFunction(Entry);
  ASSERT_NE(Frame, nullptr);
  ASSERT_TRUE(Frame->Dwarf);
  ASSERT_TRUE(Frame->Compact);
  EXPECT_EQ(Frame->Compact->NativeEncoding & macho_unwind::kModeMask,
            macho_unwind::kARM64ModeDwarf);
  const auto &FDE = *Frame->Dwarf;
  ASSERT_EQ(FDE.InitialLocation, Entry);
  const auto *Code = Image.readVA(Entry, FDE.AddressRange);
  ASSERT_NE(Code, nullptr);
  const auto StateAt =
      [&](uint64_t Off) -> std::optional<dwarf_eh::AArch64ReturnAddressState> {
    if (!Recovered.empty())
      return aarch64UnwindRowAt(Recovered, Off).Signing.State;
    const auto At = queryAArch64UnwindSigning(Image, *Frame, Entry + Off);
    return At.DwarfStateAtPC ? At.DwarfStateAtPC->State : std::nullopt;
  };
  bool Signed = false;
  unsigned Signs = 0, Auths = 0;
  for (uint64_t Off = 0; Off < FDE.AddressRange; Off += aarch64::kInsnSize) {
    ASSERT_GE(FDE.AddressRange - Off, aarch64::kInsnSize);
    const auto State = StateAt(Off);
    ASSERT_TRUE(State);
    EXPECT_EQ(State->Kind, Signed ? dwarf_eh::AArch64RASigningKind::SP
                                  : dwarf_eh::AArch64RASigningKind::Unsigned);
    if (Signed)
      EXPECT_EQ(State->Key, dwarf_eh::AArch64RAKey::B);
    const auto Word = readLE<uint32_t>(Code + Off);
    if (Word == kSignLR) {
      ++Signs;
      Signed = true;
    } else if (Word == kAuthenticateLR) {
      ++Auths;
      Signed = false;
    }
  }
  EXPECT_EQ(Signs, 1u);
  EXPECT_EQ(Auths, 1u);
  EXPECT_FALSE(Signed);
  if (Samples.empty())
    return;
  std::ifstream Input(Samples);
  ASSERT_TRUE(Input);
  for (unsigned Stage = 1; Stage <= kStages; ++Stage) {
    uint64_t Off, CFAOffset;
    unsigned IsSigned, StackMatches;
    ASSERT_TRUE(Input >> Off >> IsSigned >> CFAOffset >> StackMatches);
    ASSERT_LT(Off, FDE.AddressRange);
    EXPECT_EQ(Off % aarch64::kInsnSize, 0u);
    const auto State = StateAt(Off);
    ASSERT_TRUE(State);
    EXPECT_EQ(State->Kind, IsSigned ? dwarf_eh::AArch64RASigningKind::SP
                                    : dwarf_eh::AArch64RASigningKind::Unsigned);
    EXPECT_EQ(IsSigned, Stage != 1 && Stage != kStages);
    EXPECT_EQ(CFAOffset, Stage == kSpillStage ? kFrameSize : 0u);
    EXPECT_EQ(StackMatches, Stage == kSpillStage);
    auto Next = std::upper_bound(
        Rows.begin(), Rows.end(), Off,
        [](uint64_t PC, const StackRow &Row) { return PC < Row.Offset; });
    ASSERT_NE(Next, Rows.begin());
    const auto &Row = *std::prev(Next);
    EXPECT_EQ(Row.Depth, CFAOffset);
    EXPECT_EQ(Row.SavedLR, bool(StackMatches));
  }
  EXPECT_TRUE((Input >> std::ws).eof());
}

void checkCxxLandingPads(const BinaryImage &Image, va_t Entry, bool Dwarf,
                         va_t &TypeSlot) {
  const auto *Frame = Image.ExceptionMetadata.findFunction(Entry);
  ASSERT_NE(Frame, nullptr);
  ASSERT_EQ(bool(Frame->Dwarf), Dwarf);
  ASSERT_EQ(Frame->Personality, ExceptionPersonality::GxxPersonalityV0);
  ASSERT_TRUE(Frame->Itanium);
  ASSERT_EQ(Frame->Itanium->TypeTable.size(), 1u);
  const auto &Type = Frame->Itanium->TypeTable.front();
  ASSERT_FALSE(Type.IsCatchAll);
  TypeSlot = Type.TypeInfoSlotVA;
  ASSERT_NE(TypeSlot, 0u);
  ASSERT_TRUE(Image.DyldBindSlots.contains(TypeSlot));
  EXPECT_FALSE(Image.MachOAuthenticatedPointers.contains(TypeSlot));
  const auto *First = Image.readVA(Entry, aarch64::kInsnSize);
  ASSERT_NE(First, nullptr);
  ASSERT_EQ(readLE<uint32_t>(First), kSignLR);
  auto Rows = recoverDarwinAArch64Unwind(Image, *Frame);
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  for (unsigned I : {0u, 1u, 2u}) {
    const auto At = analyzeAArch64UnwindSigning(Image, *Frame,
                                                Entry + I * aarch64::kInsnSize);
    ASSERT_TRUE(At.InstructionStateAtPC && At.InstructionStateAtPC->State);
    EXPECT_EQ(At.InstructionStateAtPC->State->Kind,
              I ? dwarf_eh::AArch64RASigningKind::SP
                : dwarf_eh::AArch64RASigningKind::Unsigned);
  }
  bool Catch = false, Cleanup = false;
  for (const auto &Site : Frame->Itanium->CallSites) {
    if (!Site.LandingPadVA)
      continue;
    for (const auto PC : {Site.GuardedRange.Begin, Site.LandingPadVA}) {
      SCOPED_TRACE(PC);
      const auto &Row = aarch64UnwindRowAt(*Rows, PC - Entry);
      ASSERT_TRUE(Row.Signing.State);
      EXPECT_EQ(Row.Signing.State->Kind, dwarf_eh::AArch64RASigningKind::SP);
      EXPECT_EQ(Row.Signing.State->Key, dwarf_eh::AArch64RAKey::B);
      const auto Plan =
          prepareDarwinArm64eSystemCaller(Row, Frame->CodeRange, Entry);
      ASSERT_TRUE(Plan);
      if (PC == Site.LandingPadVA)
        EXPECT_TRUE(Plan->Frame.SavedLR);
    }
    if (Site.FirstActionOffset)
      Catch = true;
    else
      Cleanup = true;
  }
  EXPECT_TRUE(Catch);
  EXPECT_TRUE(Cleanup);
}
#endif

TEST(AArch64AsyncUnwind,
     PublishedFrameRecoversCallerAtFiveSignalInterruptions) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kDirectory, Dir));
  const std::filesystem::path Root(Dir.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kFrameSource).string();
  const auto Wrapper = (Root / kWrapperSource).string();
  const auto Observer = (Root / kDriverSource).string();
  const auto Driver = (Root / kDriver).string();
  const auto Input = (Root / kOriginal).string();
  const auto Output = (Root / kPublished).string();
  const auto Object = (Root / kObject).string();
  const auto OldCompanion = (Root / kOldCompanion).string();
  const auto NewCompanion = (Root / kNewCompanion).string();
  std::ofstream(OldCompanion) << kOriginalCompanion;
  std::ofstream(NewCompanion) << kAssembly << kReplacementCompanion;
  std::ofstream(Source) << kAssembly;
  std::ofstream(Wrapper) << kCaller;
  std::ofstream(Observer) << kObserver;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                kCompiler, {kCompiler, kTargetFlag, kTarget, kDylib, kHeader,
                            kExplicit, Source, OldCompanion, kOutput, Input}),
            0);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                kCompiler, {kCompiler, kTargetFlag, kTarget, kCompile,
                            NewCompanion, kOutput, Object}),
            0);
  const auto *IncludeOverride = std::getenv(kHeaderRoot);
  const auto Include =
      IncludeOverride
          ? std::string(IncludeOverride)
          : (std::filesystem::path(__FILE__).parent_path() / kSourceIncludes)
                .lexically_normal()
                .string();
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler,
                                      {kCompiler, kTargetFlag, kTarget,
                                       kOptimize, kIncludeFlag, Include,
                                       Observer, Wrapper, kOutput, Driver}),
            0);
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Original = Reader->load(Input);
  ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
  const auto Sym = llvm::find_if(Original->Symbols, [](const Symbol &S) {
    return S.IsFunc && S.Name == kNativeName;
  });
  ASSERT_NE(Sym, Original->Symbols.end());
  const va_t Entry = Sym->Addr;
  std::vector<StackRow> OriginalRows, PublishedRows;
  ASSERT_NO_FATAL_FAILURE(readStackRows(*Original, Entry, OriginalRows));
  ASSERT_NO_FATAL_FAILURE(checkFrame(*Original, Entry, {}, OriginalRows));
  auto Bytes = llvm::MemoryBuffer::getFile(Object);
  ASSERT_TRUE(bool(Bytes));
  const auto Companion = llvm::find_if(Original->Symbols, [](const Symbol &S) {
    return S.IsFunc && S.Name == kCompanionName;
  });
  ASSERT_NE(Companion, Original->Symbols.end());
  const SourcePatchMapping Mappings[] = {{kStagesMapping, Entry},
                                         {kMapping, Companion->Addr}};
  auto Patch = installMachOObjectPatch((*Bytes)->getBuffer(), *Original,
                                       Mappings, {}, Output, {});
  ASSERT_TRUE(bool(Patch)) << llvm::toString(Patch.takeError());
  ASSERT_EQ(Patch->PatchedFunctions.size(), std::size(Mappings));
  const auto Stages =
      llvm::find_if(Patch->PatchedFunctions,
                    [&](const auto &S) { return S.OriginalVA == Entry; });
  ASSERT_NE(Stages, Patch->PatchedFunctions.end());
  const va_t Owner = Stages->OwnerVA;
  ASSERT_GT(Owner, Entry);
  auto Published = Reader->load(Output);
  ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());
  ASSERT_NO_FATAL_FAILURE(readStackRows(*Published, Owner, PublishedRows));
  ASSERT_EQ(OriginalRows, PublishedRows);
  const auto *OldFrame = Original->ExceptionMetadata.findFunction(Entry);
  const auto *NewFrame = Published->ExceptionMetadata.findFunction(Owner);
  ASSERT_TRUE(OldFrame && OldFrame->Dwarf && NewFrame && NewFrame->Dwarf);
  const uint64_t Size = OldFrame->Dwarf->AddressRange;
  ASSERT_EQ(NewFrame->Dwarf->InitialLocation, Owner);
  ASSERT_EQ(NewFrame->Dwarf->AddressRange, Size);
  const auto *Before = Original->readVA(Entry, Size);
  const auto *After = Published->readVA(Owner, Size);
  ASSERT_TRUE(Before && After);
  ASSERT_EQ(llvm::ArrayRef(Before, Size), llvm::ArrayRef(After, Size));
  auto Recovered = recoverDarwinAArch64Unwind(*Published, *NewFrame);
  ASSERT_TRUE(bool(Recovered)) << llvm::toString(Recovered.takeError());
  for (uint64_t Off = 0; Off < Size; Off += aarch64::kInsnSize) {
    const auto &Implicit = aarch64UnwindRowAt(*Recovered, Off).Signing;
    const auto Explicit =
        queryAArch64UnwindSigning(*Original, *OldFrame, Entry + Off);
    ASSERT_TRUE(Implicit.State && Explicit.DwarfStateAtPC &&
                Explicit.DwarfStateAtPC->State)
        << Off;
    EXPECT_EQ(Implicit.State->Kind, Explicit.DwarfStateAtPC->State->Kind)
        << Off;
    if (Implicit.State->Kind != dwarf_eh::AArch64RASigningKind::Unsigned)
      EXPECT_EQ(Implicit.State->Key, Explicit.DwarfStateAtPC->State->Key);
  }
  if (std::getenv(kExecute)) {
    const auto WriteRules = [&](const std::filesystem::path &Path,
                                llvm::ArrayRef<StackRow> Rules,
                                bool Installed) {
      std::vector<std::pair<StackRow, bool>> Resolved;
      for (uint64_t Off = 0; Off < Size; Off += aarch64::kInsnSize) {
        dwarf_eh::AArch64RAQueryResult Signing;
        StackRow Row{};
        if (Installed) {
          const auto &Rule = aarch64UnwindRowAt(*Recovered, Off);
          const auto Plan =
              prepareDarwinArm64eSystemCaller(Rule, NewFrame->CodeRange, Owner);
          if (!Plan || Plan->Frame.CFAFromFP || Plan->Frame.SavedFP ||
              (Plan->Frame.SavedLR &&
               Plan->Frame.LROffset != -int(sizeof(uint64_t))) ||
              Plan->Count > 1 ||
              (Plan->Count &&
               (Plan->Saved[0].Register != NDArm64eFirstPreservedGPR ||
                Plan->Saved[0].CFAOffset != -int(kFrameSize))))
            return false;
          Row = {Off, int32_t(Plan->Frame.CFAOffset), Plan->Frame.SavedLR,
                 Plan->Count != 0};
          Signing = Rule.Signing;
        } else {
          const auto At =
              queryAArch64UnwindSigning(*Original, *OldFrame, Entry + Off);
          if (!At.DwarfStateAtPC)
            return false;
          Signing = *At.DwarfStateAtPC;
          Row = *std::prev(
              llvm::upper_bound(Rules, Off, [](uint64_t PC, const StackRow &R) {
                return PC < R.Offset;
              }));
          Row.Offset = Off;
        }
        if (!Signing.State)
          return false;
        const auto &State = *Signing.State;
        const bool Signed = State.Kind == dwarf_eh::AArch64RASigningKind::SP &&
                            State.Key == dwarf_eh::AArch64RAKey::B;
        if (!Signed && State.Kind != dwarf_eh::AArch64RASigningKind::Unsigned)
          return false;
        if (Resolved.empty() || Resolved.back().second != Signed ||
            Resolved.back().first.Depth != Row.Depth ||
            Resolved.back().first.SavedLR != Row.SavedLR ||
            Resolved.back().first.SavedX19 != Row.SavedX19)
          Resolved.emplace_back(Row, Signed);
      }
      std::ofstream Out(Path);
      Out << Resolved.size() << '\n';
      for (const auto &[Row, Signed] : Resolved)
        Out << Row.Offset << ' ' << Row.Depth << ' ' << Row.SavedLR << ' '
            << Signed << ' ' << Row.SavedX19 << '\n';
      Out.close();
      return bool(Out);
    };
    const auto OriginalRules = (Root / kOriginalRules).string();
    const auto InstalledRules = (Root / kPublishedRules).string();
    ASSERT_TRUE(WriteRules(OriginalRules, OriginalRows, false));
    ASSERT_TRUE(WriteRules(InstalledRules, {}, true));
    const auto OriginalSamples = (Root / kOriginalSamples).string();
    const auto InstalledSamples = (Root / kInstalledSamples).string();
    ASSERT_EQ(
        llvm::sys::ExecuteAndWait(Driver, {Driver, Input, std::to_string(0),
                                           std::to_string(kOldValue),
                                           OriginalSamples, OriginalRules}),
        0);
    ASSERT_NO_FATAL_FAILURE(
        checkFrame(*Original, Entry, OriginalSamples, OriginalRows));
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  Driver, {Driver, Output, std::to_string(Owner - Entry),
                           std::to_string(kNewValue), InstalledSamples,
                           InstalledRules}),
              0);
    ASSERT_NO_FATAL_FAILURE(checkFrame(*Published, Owner, InstalledSamples,
                                       PublishedRows, *Recovered));
    for (auto [Path, Property] :
         {std::pair(OriginalSamples, kOriginalRows),
          std::pair(InstalledSamples, kPublishedRows)}) {
      auto Rows = llvm::MemoryBuffer::getFile(Path);
      ASSERT_TRUE(bool(Rows));
      RecordProperty(Property, (*Rows)->getBuffer().str());
    }
    RecordProperty(kSamples, 2 * kStages);
    RecordProperty(kParents, 2 * kStages);
    RecordProperty(kTrampolines, 2 * kStages);
    RecordProperty(kSystemSteps, 2 * kStages);
  }
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64AsyncUnwind,
     CompilerCompactFramesRecoverTheSavedCallerWithoutDwarf) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kDirectory, Dir));
  const std::filesystem::path Root(Dir.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kCompactSource).string();
  const auto Dylib = (Root / kOriginal).string();
  std::ofstream(Source) << kCompactBody;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                kCompiler, {kCompiler, kTargetFlag, kTarget, kOptimize,
                            kFramePointer, kDylib, Source, kOutput, Dylib}),
            0);
  auto Image = Loader::create(BinaryFormat::MachO)->load(Dylib);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (const char *Name : {kCompactParent, kCompactLeaf}) {
    const auto Sym = llvm::find_if(Image->Symbols, [&](const Symbol &S) {
      return S.IsFunc && S.Name == Name;
    });
    ASSERT_NE(Sym, Image->Symbols.end());
    const auto *Frame = Image->ExceptionMetadata.findFunction(Sym->Addr);
    ASSERT_TRUE(Frame && Frame->Compact && !Frame->Dwarf);
    auto Rows = recoverDarwinAArch64Unwind(*Image, *Frame);
    ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
    bool SawSavedLR = false;
    for (const auto &R : *Rows) {
      auto Plan = prepareDarwinArm64eSystemCaller(R, Frame->Compact->CodeRange,
                                                  Sym->Addr);
      ASSERT_TRUE(Plan) << R.CodeOffset;
      if (Plan->Frame.SavedLR) {
        SawSavedLR = true;
        EXPECT_TRUE(Plan->Frame.SavedFP);
        EXPECT_EQ(Plan->Frame.LROffset, -int(sizeof(uint64_t)));
        EXPECT_EQ(Plan->Frame.Signing, NDArm64eSignedIBWithCFA);
      }
    }
    EXPECT_EQ(SawSavedLR, Name == kCompactParent);
    ASSERT_TRUE(Rows->front().Signing.State);
    EXPECT_EQ(Rows->front().Signing.State->Kind,
              dwarf_eh::AArch64RASigningKind::Unsigned);
  }
  if (std::getenv(kExecute)) {
    const auto DriverSource = (Root / kDriverSource).string();
    const auto Driver = (Root / kDriver).string();
    std::ofstream(DriverSource) << kCompactDriver;
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  kCompiler, {kCompiler, kTargetFlag, kTarget, kOptimize,
                              DriverSource, kOutput, Driver}),
              0);
    ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, Dylib}), 0);
    RecordProperty(kCompactCalls, kCompactInputs);
  }
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64AsyncUnwind, CompilerCatchAndCleanupRecoverTheSavedSignedLR) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kDirectory, Dir));
  const std::filesystem::path Root(Dir.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kCatchFile).string();
  const auto Provider = (Root / kCatchImports).string();
  const auto ProviderSource = (Root / kCatchImportsFile).string();
  const auto DriverSource = (Root / kCatchDriverFile).string();
  const auto Driver = (Root / kDriver).string();
  const auto Dylib = (Root / kOriginal).string();
  const auto Output = (Root / kPublished).string();
  const auto Object = (Root / kObject).string();
  std::ofstream(Source) << kCatchBody;
  std::ofstream(ProviderSource) << kCatchProvider;
  std::ofstream(DriverSource) << kCatchDriver;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCxxCompiler,
                                      {kCxxCompiler, kTargetFlag, kTarget,
                                       kCXXStandard, kOptimize, kDylib,
                                       ProviderSource, kOutput, Provider}),
            0);
  unsigned Paths = 0;
  struct Profile {
    const char *Opt;
    bool Writable;
    bool Dwarf;
  };
  const Profile Profiles[] = {{kNoOptimize, false, false},
                              {kOptimize, false, false},
                              {kOptimize, true, false},
                              {kOptimize, false, true}};
  for (const auto &[Opt, Writable, Dwarf] : Profiles) {
    SCOPED_TRACE(Opt);
    SCOPED_TRACE(Writable);
    SCOPED_TRACE(Dwarf);
    llvm::SmallVector<llvm::StringRef> Compile{
        kCxxCompiler, kTargetFlag, kTarget, kCXXStandard, Opt, kFramePointer};
    if (Dwarf)
      Compile.push_back(kCatchDwarf);
    auto Link = Compile;
    Link.push_back(kDylib);
    if (Writable)
      Link.push_back(kWritableGOT);
    Link.append({Source, Provider, kOutput, Dylib});
    ASSERT_EQ(llvm::sys::ExecuteAndWait(kCxxCompiler, Link), 0);
    auto Image = Loader::create(BinaryFormat::MachO)->load(Dylib);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    const auto *Sym = Image->findSymbol(kCatchSymbol);
    ASSERT_NE(Sym, nullptr);
    va_t OriginalTypeSlot = 0;
    ASSERT_NO_FATAL_FAILURE(
        checkCxxLandingPads(*Image, Sym->Addr, Dwarf, OriginalTypeSlot));
    Compile.append({kCatchReplacement, kCompile, Source, kOutput, Object});
    ASSERT_EQ(llvm::sys::ExecuteAndWait(kCxxCompiler, Compile), 0);
    auto Bytes = llvm::MemoryBuffer::getFile(Object);
    ASSERT_TRUE(bool(Bytes));
    const SourcePatchMapping Mapping{kCatchMapping, Sym->Addr};
    auto Patch = installMachOObjectPatch((*Bytes)->getBuffer(), *Image,
                                         {Mapping}, {}, Output, {});
    ASSERT_TRUE(bool(Patch)) << llvm::toString(Patch.takeError());
    ASSERT_EQ(Patch->PatchedFunctions.size(), 1u);
    const va_t Owner = Patch->PatchedFunctions.front().OwnerVA;
    ASSERT_NE(Owner, Sym->Addr);
    auto Published = Loader::create(BinaryFormat::MachO)->load(Output);
    ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());
    va_t PublishedTypeSlot = 0;
    ASSERT_NO_FATAL_FAILURE(
        checkCxxLandingPads(*Published, Owner, Dwarf, PublishedTypeSlot));
    const auto SlidTypeSlot = OriginalTypeSlot + Patch->OriginalSectionSlide;
    if (Writable)
      EXPECT_NE(PublishedTypeSlot, SlidTypeSlot);
    else
      EXPECT_EQ(PublishedTypeSlot, SlidTypeSlot);
    if (std::getenv(kExecute)) {
      ASSERT_EQ(llvm::sys::ExecuteAndWait(kCxxCompiler,
                                          {kCxxCompiler, kTargetFlag, kTarget,
                                           kCXXStandard, Opt, DriverSource,
                                           kOutput, Driver}),
                0);
      ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, Dylib}), 0);
      Paths += kCxxInputs;
      ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, Output, kPublished}),
                0);
      Paths += kCxxInputs;
    }
    ASSERT_TRUE(std::filesystem::remove(Output));
  }
  RecordProperty(kCxxPaths, Paths);
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64AsyncUnwind, InstructionAddressRecipesUseTheActualSigningPC) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kDirectory, Dir));
  const std::filesystem::path Root(Dir.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto *IncludeOverride = std::getenv(kHeaderRoot);
  const auto Include =
      IncludeOverride
          ? std::string(IncludeOverride)
          : (std::filesystem::path(__FILE__).parent_path() / kSourceIncludes)
                .lexically_normal()
                .string();
  const auto Source =
      (std::filesystem::path(Include).parent_path() / kCursorSource).string();
  const auto Driver = (Root / kDriver).string();
  unsigned PACMProfiles = 0, ExplicitProfiles = 0;
  for (const auto &[Language, Standard] :
       {std::pair(kCLanguage, kCStandard),
        std::pair(kCXXLanguage, kCXXStandard)}) {
    for (const char *Optimize : {kNoOptimize, kOptimize}) {
      ASSERT_EQ(llvm::sys::ExecuteAndWait(
                    kCompiler, {kCompiler, kTargetFlag, kTarget, Standard,
                                kAllWarnings, kExtraWarnings, kErrorWarnings,
                                Optimize, kIncludeFlag, Include, kLanguageFlag,
                                Language, Source, kOutput, Driver}),
                0);
      if (!std::getenv(kExecute))
        continue;
      ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, kPACMMode}), 0);
      ++PACMProfiles;
      const int Explicit =
          llvm::sys::ExecuteAndWait(Driver, {Driver, kExplicitMode});
      ASSERT_TRUE(Explicit == 0 || Explicit == kUnavailableCode);
      ExplicitProfiles += Explicit == 0;
    }
  }
  RecordProperty(kCursorPACMProfiles, PACMProfiles);
  RecordProperty(kCursorExplicitProfiles, ExplicitProfiles);
#else
  GTEST_SKIP() << kUnavailable;
#endif
}
} // namespace
