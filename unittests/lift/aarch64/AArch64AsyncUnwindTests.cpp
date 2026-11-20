//===- AArch64AsyncUnwindTests.cpp - Interrupted native return signing ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/AArch64UnwindRecovery.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/decode/Decoder.h"
#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/FunctionBody.h"
#include "neverd/loader/ReturnAddressSigning.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugFrame.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
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

void checkLanguageLandingPads(
    const BinaryImage &Image, va_t Entry, bool Dwarf, bool StackArgs,
    va_t &TypeSlot,
    ExceptionPersonality Personality = ExceptionPersonality::GxxPersonalityV0) {
  const auto *Frame = Image.ExceptionMetadata.findFunction(Entry);
  ASSERT_NE(Frame, nullptr);
  ASSERT_EQ(bool(Frame->Dwarf), Dwarf);
  if (Dwarf)
    EXPECT_EQ(llvm::any_of(Frame->Dwarf->Instructions,
                           [](const CFIInstruction &I) {
                             return I.Kind == CFIOpKind::GnuArgsSize &&
                                    I.Offset == kCxxCallArgumentBytes;
                           }),
              StackArgs);
  ASSERT_EQ(Frame->Personality, Personality);
  ASSERT_TRUE(Frame->Itanium);
  const auto &Types = Frame->Itanium->TypeTable;
  if (Personality == ExceptionPersonality::GxxPersonalityV0)
    ASSERT_EQ(Types.size(), 1u);
  const auto Typed = [](const auto &Type) { return !Type.IsCatchAll; };
  ASSERT_EQ(llvm::count_if(Types, Typed), 1);
  TypeSlot = llvm::find_if(Types, Typed)->TypeInfoSlotVA;
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
  const va_t OldEntry = Entry + Patch->OriginalSectionSlide;
  const auto OldCompanionEntry = Companion->Addr + Patch->OriginalSectionSlide;
  const auto *CompactLeaf =
      Original->ExceptionMetadata.findFunction(Companion->Addr);
  const auto *CompactEntry =
      Published->ExceptionMetadata.findFunction(OldCompanionEntry);
  ASSERT_TRUE(CompactLeaf && CompactLeaf->Compact && !CompactLeaf->Dwarf);
  ASSERT_TRUE(CompactEntry && CompactEntry->Compact && !CompactEntry->Dwarf);
  EXPECT_EQ(CompactLeaf->Compact->NativeEncoding,
            macho_unwind::kARM64ModeFrameless);
  EXPECT_EQ(CompactEntry->Compact->NativeEncoding,
            CompactLeaf->Compact->NativeEncoding);
  const auto Forward =
      machOLiftedSource(*Published, OldCompanionEntry, InvalidVA);
  ASSERT_TRUE(Forward && Forward->ForwardTarget);
  const auto *Trampoline = Published->ExceptionMetadata.findFunction(OldEntry);
  ASSERT_TRUE(Trampoline && Trampoline->Dwarf);
  ASSERT_EQ(Trampoline->CodeRange.Begin, OldEntry);
  auto EntryRows = recoverDarwinAArch64Unwind(*Published, *Trampoline);
  ASSERT_TRUE(bool(EntryRows)) << llvm::toString(EntryRows.takeError());
  const auto &First = EntryRows->front();
  ASSERT_TRUE(First.IsForwardingEntry);
  const auto EntryPlan =
      prepareDarwinArm64eSystemCaller(First, Trampoline->CodeRange, OldEntry);
  ASSERT_TRUE(EntryPlan);
  ASSERT_TRUE(EntryPlan->Vectors.Complete);
  ASSERT_FALSE(EntryPlan->Frame.CFAFromFP || EntryPlan->Frame.SavedFP ||
               EntryPlan->Frame.SavedLR || EntryPlan->Count);
  ASSERT_EQ(EntryPlan->Frame.Signing, NDArm64eUnsigned);
  ASSERT_EQ(EntryPlan->Frame.CFAOffset, 0);
  const auto EntrySite = prepareDarwinArm64eExceptionSite(
      *Published, *Trampoline, *EntryRows, OldEntry, OldEntry);
  ASSERT_TRUE(EntrySite);
  EXPECT_EQ(EntrySite->Begin, OldEntry);
  EXPECT_EQ(EntrySite->End, OldEntry + aarch64::kInsnSize);
  EXPECT_FALSE(EntrySite->AtProtectedCall);
  EXPECT_FALSE(prepareDarwinArm64eExceptionSite(
      *Published, *Trampoline, *EntryRows, EntrySite->End, OldEntry));
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
    const auto EntryRules = (Root / kEntryRules).string();
    const auto EntrySamples = (Root / kEntrySamples).string();
    std::ofstream(EntryRules)
        << 1 << '\n'
        << First.CodeOffset << ' ' << EntryPlan->Frame.CFAOffset << ' '
        << EntryPlan->Frame.SavedLR << ' '
        << (EntryPlan->Frame.Signing == NDArm64eSignedIBWithCFA) << ' '
        << EntryPlan->Count << '\n';
    ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver,
                                        {Driver, Output, std::to_string(0),
                                         std::to_string(kNewValue),
                                         EntrySamples, EntryRules, kEntryMode}),
              0);
    std::ifstream Sample(EntrySamples);
    uint64_t Offset, Depth;
    int Signed, Saved;
    ASSERT_TRUE(Sample >> Offset >> Signed >> Depth >> Saved);
    EXPECT_EQ(Offset, 0u);
    EXPECT_EQ(Depth, 0u);
    EXPECT_EQ(Signed, 0);
    EXPECT_EQ(Saved, 0);
    EXPECT_TRUE((Sample >> std::ws).eof());
    RecordProperty(kEntrySteps, 1);
    const auto Fields = [](const CFIInstruction &I) {
      return std::tie(I.Kind, I.CodeOffset, I.Register, I.Register2, I.Offset,
                      I.Expression, I.OperandBytes, I.RAState);
    };
    ASSERT_TRUE(llvm::equal(
        OldFrame->Dwarf->Instructions, Trampoline->Dwarf->Instructions,
        [&](const auto &A, const auto &B) { return Fields(A) == Fields(B); }));
    for (const auto &Path : {Input, Output}) {
      SCOPED_TRACE(Path);
      for (llvm::StringRef Mode : {kRawEntryMode, kCompactEntryMode}) {
        SCOPED_TRACE(Mode.str());
        ASSERT_EQ(
            llvm::sys::ExecuteAndWait(
                Driver, {Driver, Path, std::to_string(0),
                         std::to_string(Path == Input ? kOldValue : kNewValue),
                         EntrySamples, Mode, Mode}),
            0);
        std::ifstream Raw(EntrySamples);
        int Visited, Exact, MatchedSP;
        unsigned Lookup;
        ASSERT_TRUE(Raw >> Visited >> Exact >> MatchedSP >> Lookup);
        EXPECT_TRUE((Raw >> std::ws).eof());
        EXPECT_TRUE(Visited == 0 || Visited == 1);
        EXPECT_EQ(Exact, 1);
        EXPECT_TRUE(MatchedSP == 0 || MatchedSP == 1);
        EXPECT_LE(Lookup, NDArm64eEntryLookupInteriorByte);
        if (Path == Output && Mode == kCompactEntryMode)
          RecordProperty(kCompactOldEntry, Exact);
        if (Path == Output && Mode == kRawEntryMode) {
          RecordProperty(kRawEntryWalk, 1);
          RecordProperty(kRawEntryVisit, Visited);
          RecordProperty(kExactEntryStep, Exact);
          RecordProperty(kRawEntrySP, MatchedSP);
          RecordProperty(kEntryLookupOffset, Lookup);
        }
      }
    }
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
  unsigned Paths = 0, UnwinderTraps = 0;
  struct Profile {
    const char *Opt;
    bool Writable;
    bool Dwarf;
    bool StackArgs = false;
  };
  const Profile Profiles[] = {{kNoOptimize, false, false},
                              {kOptimize, false, false},
                              {kOptimize, true, false},
                              {kOptimize, false, true},
                              {kOptimize, false, true, true}};
  for (const auto &[Opt, Writable, Dwarf, StackArgs] : Profiles) {
    SCOPED_TRACE(Opt);
    SCOPED_TRACE(Writable);
    SCOPED_TRACE(Dwarf);
    SCOPED_TRACE(StackArgs);
    llvm::SmallVector<llvm::StringRef> Compile{
        kCxxCompiler, kTargetFlag, kTarget, kCXXStandard, Opt, kFramePointer};
    if (StackArgs)
      Compile.push_back(kCatchStackArgs);
    else if (Dwarf)
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
    ASSERT_NO_FATAL_FAILURE(checkLanguageLandingPads(
        *Image, Sym->Addr, Dwarf, StackArgs, OriginalTypeSlot));
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
    ASSERT_NO_FATAL_FAILURE(checkLanguageLandingPads(
        *Published, Owner, Dwarf, StackArgs, PublishedTypeSlot));
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
      const auto Run = [&](const std::string &File, unsigned Adjustment,
                           std::optional<unsigned> Input = std::nullopt) {
        const auto Delta = std::to_string(Adjustment);
        const auto Case = Input ? std::to_string(*Input) : std::string();
        llvm::SmallVector<llvm::StringRef> Args{Driver, File, Delta};
        if (Input)
          Args.push_back(Case);
        return llvm::sys::ExecuteAndWait(Driver, Args);
      };
      if (StackArgs) {
        for (unsigned I = 0; I < kExceptionInputs; ++I) {
          SCOPED_TRACE(I);
          const int Original = Run(Dylib, 0, I);
          ASSERT_TRUE(Original == 0 || (I && Original == kUnwinderPACTrapExit))
              << Original;
          ASSERT_EQ(Run(Output, 1, I), Original);
          if (Original)
            UnwinderTraps += 2;
          else
            Paths += 2;
        }
      } else {
        ASSERT_EQ(Run(Dylib, 0), 0);
        ASSERT_EQ(Run(Output, 1), 0);
        Paths += 2 * kExceptionInputs;
      }
    }
    ASSERT_TRUE(std::filesystem::remove(Output));
  }
  RecordProperty(kCxxPaths, Paths);
  RecordProperty(kCxxTraps, UnwinderTraps);
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64AsyncUnwind, SignalReturnResumesCallerWithRecoveredVectorLanes) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kDirectory, Dir));
  const std::filesystem::path Root(Dir.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kFrameSource).string();
  const auto Wrapper = (Root / kWrapperSource).string();
  const auto DriverSource = (Root / kDriverSource).string();
  const auto Driver = (Root / kDriver).string();
  const auto Input = (Root / kOriginal).string();
  const auto Object = (Root / kObject).string();
  const auto Output = (Root / kPublished).string();
  std::ofstream(Source) << kResumeBody;
  std::ofstream(Wrapper) << kResumeWrapper;
  std::ofstream(DriverSource) << kResumeDriver;
  const auto *Override = std::getenv(kHeaderRoot);
  const auto Include =
      Override
          ? std::string(Override)
          : (std::filesystem::path(__FILE__).parent_path() / kSourceIncludes)
                .lexically_normal()
                .string();
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(kCompiler, {kCompiler, kTargetFlag, kTarget,
                                            kDylib, Source, kOutput, Input}),
      0);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                kCompiler, {kCompiler, kTargetFlag, kTarget, kResumeChanged,
                            kCompile, Source, kOutput, Object}),
            0);
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(
          kCompiler, {kCompiler, kTargetFlag, kTarget, kOptimize, kIncludeFlag,
                      Include, kAllWarnings, kExtraWarnings, kErrorWarnings,
                      DriverSource, Wrapper, kOutput, Driver}),
      0);
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Original = Reader->load(Input);
  ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
  const auto *Symbol = Original->findSymbol(kResumeSymbol);
  ASSERT_NE(Symbol, nullptr);
  const SourcePatchMapping Map{kResumeMapping, Symbol->Addr};
  const auto Bytes = llvm::MemoryBuffer::getFile(Object);
  ASSERT_TRUE(bool(Bytes));
  auto Patch = installMachOObjectPatch((*Bytes)->getBuffer(), *Original, {Map},
                                       {}, Output, {});
  ASSERT_TRUE(bool(Patch)) << llvm::toString(Patch.takeError());
  ASSERT_EQ(Patch->PatchedFunctions.size(), 1u);
  auto Published = Reader->load(Output);
  ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());

  unsigned Count = 0;
  for (bool Installed : {false, true}) {
    SCOPED_TRACE(Installed);
    const BinaryImage &Image = Installed ? *Published : *Original;
    const auto Entry =
        Installed ? Patch->PatchedFunctions.front().OwnerVA : Symbol->Addr;
    const auto *Frame = Image.ExceptionMetadata.findFunction(Entry);
    ASSERT_NE(Frame, nullptr);
    auto Rows = recoverDarwinAArch64Unwind(Image, *Frame);
    ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
    const auto Rules = (Root / kOriginalRules).string();
    bool Found = false;
    for (const auto &[Index, Row] : llvm::enumerate(*Rows)) {
      const auto Plan =
          prepareDarwinArm64eSystemCaller(Row, Frame->CodeRange, Entry);
      if (!Plan || !Plan->Vectors.Complete ||
          Plan->Vectors.LowMask != (1u << NDArm64ePreservedDCount) - 1)
        continue;
      ASSERT_FALSE(Found);
      Found = true;
      ASSERT_EQ(Plan->Vectors.HighMask,
                (1u << (NDArm64ePreservedDCount / 2)) - 1);
      ASSERT_EQ(Plan->Frame.Signing, NDArm64eSignedIBWithCFA);
      ASSERT_FALSE(Plan->Frame.CFAFromFP || Plan->Frame.SavedFP);
      ASSERT_TRUE(Plan->Frame.SavedLR);
      ASSERT_EQ(Plan->Frame.CFAOffset, kResumeDepth);
      ASSERT_EQ(Plan->Count, 1u);
      ASSERT_EQ(Plan->Saved[0].Register, NDArm64eFirstPreservedGPR);
      std::ofstream Out(Rules);
      const auto End = Index + 1 < Rows->size() ? (*Rows)[Index + 1].CodeOffset
                                                : Frame->CodeRange.size();
      Out << Row.CodeOffset << ' ' << End << ' ' << Plan->Frame.CFAOffset << ' '
          << Plan->Frame.LROffset << ' ' << Plan->Saved[0].CFAOffset << ' '
          << Plan->Vectors.LowMask << ' ' << Plan->Vectors.HighMask << '\n';
      for (unsigned D = 0; D < NDArm64ePreservedDCount; ++D)
        Out << Plan->Vectors.LowOffsets[D] << ' '
            << Plan->Vectors.HighOffsets[D] << '\n';
      Out.close();
      ASSERT_TRUE(Out);
    }
    ASSERT_TRUE(Found);
    if (std::getenv(kExecute)) {
      const auto *Public = Image.findSymbol(kResumeSymbol);
      ASSERT_NE(Public, nullptr);
      ASSERT_EQ(llvm::sys::ExecuteAndWait(
                    Driver, {Driver, Installed ? Output : Input,
                             std::to_string(Entry - Public->Addr),
                             std::to_string(Installed ? 2 : 1), Rules}),
                0);
      Count += kResumeProfiles;
    }
  }
  RecordProperty(kResumeCount, Count);
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

enum class LanguageFrame {
  Call,
  IndexedNonCall,
  CompactNonCall,
  FramelessNonCall,
  LiveLRNonCall,
  LiveLRCatch,
  CompactLiveLR,
  CompactLiveLRCatch,
  ExplicitLPStart,
  TypedExplicitLPStart,
  EntryLandingPad
};

static void verifySignalLanguageCleanup(
    bool ObjectiveC, LanguageFrame Kind = LanguageFrame::Call,
    bool Lifted = false,
    NDDarwinArm64eSigning Signing = NDArm64eSignedIBWithCFA) {
#if defined(__APPLE__) && defined(__aarch64__)
  const bool ExplicitPC = Signing == NDArm64eSignedIAWithCFAPC ||
                          Signing == NDArm64eSignedIBWithCFAPC;
  const bool PACM = Signing == NDArm64eSignedIAWithCFAPACM ||
                    Signing == NDArm64eSignedIBWithCFAPACM;
  const bool PCSigning = ExplicitPC || PACM;
  const bool NonCall = Kind != LanguageFrame::Call;
  const bool EntryPad = Kind == LanguageFrame::EntryLandingPad;
  const bool Explicit = Kind == LanguageFrame::ExplicitLPStart ||
                        Kind == LanguageFrame::TypedExplicitLPStart || EntryPad;
  const bool Typed = Kind == LanguageFrame::TypedExplicitLPStart;
  const bool CompactLiveLR = Kind == LanguageFrame::CompactLiveLR ||
                             Kind == LanguageFrame::CompactLiveLRCatch;
  const bool LocalCatch = Kind == LanguageFrame::LiveLRCatch ||
                          Kind == LanguageFrame::CompactLiveLRCatch;
  const bool LiveLR =
      Kind == LanguageFrame::LiveLRNonCall || LocalCatch || CompactLiveLR;
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kDirectory, Dir));
  const std::filesystem::path Root(Dir.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / (NonCall      ? kSignalNonCallSource
                               : ObjectiveC ? kSignalObjCSource
                                            : kSignalScopeSource))
                          .string();
  const auto DriverSource =
      (Root / (ObjectiveC ? kSignalObjCDriver : kSignalScopeDriverFile))
          .string();
  const auto Driver = (Root / kDriver).string();
  const auto Input = (Root / kOriginal).string();
  const auto Object = (Root / kObject).string();
  const auto Output = (Root / kPublished).string();
  const auto Rules = (Root / kOriginalRules).string();
  std::ofstream(Source) << (NonCall      ? kSignalNonCallBody
                            : ObjectiveC ? kSignalObjCBody
                                         : kSignalScopeBody);
  std::ofstream(DriverSource) << kSignalScopeDriver;
  const auto *Override = std::getenv(kHeaderRoot);
  const auto Include =
      Override
          ? std::string(Override)
          : (std::filesystem::path(__FILE__).parent_path() / kSourceIncludes)
                .lexically_normal()
                .string();
  llvm::SmallVector<llvm::StringRef> DriverArgs{
      kCxxCompiler, kTargetFlag, kTarget,      kCXXStandard,   kOptimize,
      kIncludeFlag, Include,     kAllWarnings, kExtraWarnings, kErrorWarnings,
      DriverSource, kOutput,     Driver};
  if (ObjectiveC)
    DriverArgs.append({kObjCExceptions, kObjCFramework, kFoundation});
  if (NonCall)
    DriverArgs.push_back(kSignalNonCallDriver);
  if (LocalCatch)
    DriverArgs.push_back(kSignalLocalCatch);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCxxCompiler, DriverArgs), 0);
  auto Reader = Loader::create(BinaryFormat::MachO);
  if (LiveLR) {
    auto DriverImage = Reader->load(Driver);
    ASSERT_TRUE(bool(DriverImage)) << llvm::toString(DriverImage.takeError());
    const auto *Bridge = DriverImage->findSymbol(kSignalLiveLRBridge);
    ASSERT_NE(Bridge, nullptr);
    const auto *Frame =
        DriverImage->ExceptionMetadata.findFunction(Bridge->Addr);
    ASSERT_TRUE(Frame && Frame->Dwarf);
    const auto *CIE =
        DriverImage->ExceptionMetadata.findCIE(Frame->Dwarf->CIESectionOffset);
    ASSERT_NE(CIE, nullptr);
    EXPECT_EQ(CIE->ReturnAddressRegister, NDArm64eVirtualReturnRegister);
    for (unsigned Reg : {unsigned(NDArm64eVirtualReturnRegister), kDwarfLR,
                         unsigned(NDArm64eCallerLRRegister)}) {
      EXPECT_TRUE(llvm::any_of(Frame->Dwarf->Instructions, [&](const auto &I) {
        return I.Kind == CFIOpKind::Offset && I.Register == Reg &&
               I.Offset == -int(NDArm64eLiveLRBridgeBytes) +
                               (Reg == NDArm64eVirtualReturnRegister
                                    ? 0
                                    : int(sizeof(uintptr_t)) *
                                          (Reg == kDwarfLR ? 1 : 2));
      }));
    }
    const auto *Resume = DriverImage->findSymbol(kSignalLiveLRResume);
    ASSERT_NE(Resume, nullptr);
    const auto *ResumeFrame =
        DriverImage->ExceptionMetadata.findFunction(Resume->Addr);
    ASSERT_TRUE(ResumeFrame && ResumeFrame->Dwarf);
    const auto *ResumeCIE = DriverImage->ExceptionMetadata.findCIE(
        ResumeFrame->Dwarf->CIESectionOffset);
    ASSERT_NE(ResumeCIE, nullptr);
    EXPECT_EQ(ResumeCIE->ReturnAddressRegister, NDArm64eResumeReturnRegister);
    EXPECT_TRUE(
        llvm::any_of(ResumeFrame->Dwarf->Instructions, [](const auto &I) {
          return I.Kind == CFIOpKind::DefCFA &&
                 I.Register == NDArm64eInterruptedSPRegister && I.Offset == 0;
        }));
    EXPECT_TRUE(
        llvm::any_of(ResumeFrame->Dwarf->Instructions, [](const auto &I) {
          return I.Kind == CFIOpKind::Register && I.Register == kDwarfLR &&
                 I.Register2 == NDArm64eSystemLRRegister;
        }));
  }
  unsigned Delivered = 0, StructuralImages = 0;
  bool Execute = std::getenv(kExecute);
  if (Execute && ExplicitPC) {
    const int Probe =
        llvm::sys::ExecuteAndWait(Driver, {Driver, kSignalExplicitProbe},
                                  std::nullopt, {}, kSignalDeliverySeconds);
    ASSERT_TRUE(Probe == 0 || Probe == kUnavailableCode);
    Execute = Probe == 0;
  }
  for (const auto *Opt : {kNoOptimize, kOptimize}) {
    SCOPED_TRACE(Opt);
    const auto Compile = [&](llvm::ArrayRef<llvm::StringRef> Args) {
      const auto *Compiler = ObjectiveC ? kCompiler : kCxxCompiler;
      llvm::SmallVector<llvm::StringRef> Command{Compiler, kTargetFlag, kTarget,
                                                 kFramePointer, Opt};
      if (!NonCall)
        Command.push_back(ObjectiveC ? kObjCExceptions : kCXXStandard);
      Command.append(Args.begin(), Args.end());
      if (NonCall && ObjectiveC)
        Command.push_back(kSignalObjCPersonality);
      if (Kind == LanguageFrame::CompactNonCall || Explicit)
        Command.push_back(kSignalCompactFrame);
      if (Explicit)
        Command.push_back(kSignalExplicitLPStart);
      if (Typed)
        Command.push_back(kSignalTypedLSDA);
      if (EntryPad)
        Command.push_back(kSignalEntryPad);
      if (Kind == LanguageFrame::FramelessNonCall)
        Command.push_back(kSignalFramelessFrame);
      if (LiveLR)
        Command.push_back(kSignalLiveLR);
      if (CompactLiveLR)
        Command.push_back(kSignalCompactLiveLR);
      if (Signing == NDArm64eUnsigned)
        Command.push_back(kSignalUnsignedLR);
      if (Signing == NDArm64eSignedIAWithCFA ||
          Signing == NDArm64eSignedIAWithCFAPC ||
          Signing == NDArm64eSignedIAWithCFAPACM)
        Command.push_back(kSignalIALR);
      if (PACM)
        Command.push_back(kSignalPACMLR);
      if (ExplicitPC)
        Command.push_back(kSignalExplicitLR);
      if (LocalCatch)
        Command.push_back(kSignalLocalCatch);
      if (ObjectiveC && llvm::is_contained(Args, kDylib))
        Command.append({kObjCFramework, kFoundation});
      return llvm::sys::ExecuteAndWait(Compiler, Command);
    };
    ASSERT_EQ(Compile({kDylib, Source, kOutput, Input}), 0);
    if (!Lifted)
      ASSERT_EQ(Compile({kCompile, kResumeChanged, Source, kOutput, Object}),
                0);
    auto Original = Reader->load(Input);
    ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
    const auto *Scope = Original->findSymbol(kSignalScopeSymbol);
    const auto *Tick = Original->findSymbol(kSignalTickSymbol);
    ASSERT_TRUE(Scope && Tick);
    const auto *SourceFrame =
        Original->ExceptionMetadata.findFunction(Scope->Addr);
    ASSERT_TRUE(SourceFrame && SourceFrame->Itanium);
    if (CompactLiveLR) {
      ASSERT_TRUE(SourceFrame->Compact);
      EXPECT_EQ(SourceFrame->Compact->Kind,
                CompactUnwindKind::FramelessImmediate);
      ASSERT_FALSE(SourceFrame->Dwarf);
    }
    if (Kind == LanguageFrame::CompactNonCall || Explicit) {
      ASSERT_TRUE(SourceFrame->Compact);
      ASSERT_EQ(SourceFrame->Compact->Kind, CompactUnwindKind::FramePointer);
      ASSERT_FALSE(SourceFrame->Dwarf);
    }
    if (Explicit)
      ASSERT_NE(SourceFrame->Itanium->LandingPadBase,
                SourceFrame->CodeRange.Begin);
    if (Kind == LanguageFrame::FramelessNonCall) {
      ASSERT_TRUE(SourceFrame->Dwarf);
      EXPECT_TRUE(
          llvm::none_of(SourceFrame->Dwarf->Instructions, [](const auto &Row) {
            return Row.Kind == CFIOpKind::DefCFARegister &&
                   Row.Register == aarch64_unwind::kFP;
          }));
    }
    if (Typed) {
      ASSERT_EQ(SourceFrame->Itanium->TypeTable.size(), kSignalTypeCount);
      ASSERT_EQ(SourceFrame->Itanium->Actions.size(), kSignalActionCount);
      ASSERT_EQ(SourceFrame->Itanium->ExceptionSpecs.size(), 1u);
      EXPECT_EQ(SourceFrame->Itanium->ExceptionSpecs.front().TypeIndices,
                std::vector<uint64_t>({kSignalSpecType}));
    }
    llvm::SmallVector<SourcePatchMapping> Maps{
        {kSignalScopeMapping, Scope->Addr}, {kSignalTickMapping, Tick->Addr}};
    if (CompactLiveLR && LocalCatch) {
      const auto *Catch = Original->findSymbol(kSignalCatchSymbol);
      ASSERT_NE(Catch, nullptr);
      Maps.push_back({kSignalCatchMapping, Catch->Addr});
    }
    llvm::Expected<PatchResult> Patch = [&]() -> llvm::Expected<PatchResult> {
      if (!Lifted) {
        auto Bytes = llvm::MemoryBuffer::getFile(Object);
        if (!Bytes)
          return llvm::errorCodeToError(Bytes.getError());
        return installMachOObjectPatch((*Bytes)->getBuffer(), *Original, Maps,
                                       {}, Output, {});
      }
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.PatchMode = true;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Original, Context, Options);
      if (!Result.Success)
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       Result.Error);
      MachOPatcher Patcher;
      Patcher.setImageContext(&*Original);
      auto Changed = llvm::CloneModule(*Result.LlvmModule);
      bool Corrupted = false;
      if (Explicit) {
        for (auto &Function : *Changed)
          for (auto &Block : Function)
            for (auto &Instruction : Block) {
              auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
              auto *Assembly = Call ? llvm::dyn_cast<llvm::InlineAsm>(
                                          Call->getCalledOperand())
                                    : nullptr;
              if (!Assembly)
                continue;
              auto Text = Assembly->getAsmString().str();
              const auto Marker =
                  llvm::StringRef(EntryPad ? kSignalGeneratedExplicitLSDA
                                           : kSignalGeneratedLSDA);
              const auto Offset = Text.find(Marker.str());
              if (Offset == std::string::npos)
                continue;
              Text.replace(Offset, Marker.size(), kSignalInvalidLSDA);
              Call->setCalledOperand(llvm::InlineAsm::get(
                  Assembly->getFunctionType(), Text,
                  Assembly->getConstraintString(), Assembly->hasSideEffects(),
                  Assembly->isAlignStack(), Assembly->getDialect(),
                  Assembly->canThrow()));
              Corrupted = true;
            }
      } else if (auto *Table = Changed->getNamedGlobal(
                     makeNdDataSymbol(SourceFrame->Itanium->LSDAVA))) {
        Table->replaceAllUsesWith(
            llvm::ConstantPointerNull::get(llvm::PointerType::get(Context, 0)));
        Corrupted = true;
      }
      EXPECT_TRUE(Corrupted);
      const auto Rejected = (Root / kRejectedLanguageImage).string();
      EXPECT_FALSE(
          Patcher.patch(Input, Rejected, *Changed, Original->Arch).Success);
      EXPECT_FALSE(std::filesystem::exists(Rejected));
      auto Installed =
          Patcher.patch(Input, Output, *Result.LlvmModule, Original->Arch);
      return Installed;
    }();
    ASSERT_TRUE(bool(Patch)) << llvm::toString(Patch.takeError());
    ASSERT_TRUE(Patch->Success);
    auto Published = Reader->load(Output);
    ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());
    if (Lifted && Explicit) {
      const auto *Rebuilt = Published->ExceptionMetadata.findFunction(
          Patch->PatchedFunctions.front().OwnerVA);
      ASSERT_TRUE(Rebuilt && Rebuilt->Itanium);
      EXPECT_EQ(Rebuilt->Itanium->LandingPadBase,
                Rebuilt->CodeRange.Begin -
                    (EntryPad ? NDArm64eInstructionAlignment : 0));
      EXPECT_NE(Rebuilt->Itanium->LSDAVA, SourceFrame->Itanium->LSDAVA);
      ASSERT_EQ(Rebuilt->Itanium->CallSites.size(),
                SourceFrame->Itanium->CallSites.size());
      for (size_t I = 0; I < Rebuilt->Itanium->CallSites.size(); ++I) {
        const auto &Before = SourceFrame->Itanium->CallSites[I];
        const auto &After = Rebuilt->Itanium->CallSites[I];
        EXPECT_EQ(Before.GuardedRange.Begin - SourceFrame->CodeRange.Begin,
                  After.GuardedRange.Begin - Rebuilt->CodeRange.Begin);
        if (Before.LandingPadVA)
          EXPECT_EQ(Before.LandingPadVA - SourceFrame->CodeRange.Begin,
                    After.LandingPadVA - Rebuilt->CodeRange.Begin);
        else
          EXPECT_EQ(After.LandingPadVA, 0u);
      }
    }
    // This coordinate-boundary fixture qualifies serialization only. Its
    // entry-point handler deliberately has no executable cleanup contract.
    if (EntryPad) {
      ASSERT_TRUE(std::filesystem::remove(Output));
      continue;
    }
    for (bool Installed : {false, true}) {
      const auto &Image = Installed ? *Published : *Original;
      enum class InterruptedSite { Leaf, Call, Prefix, NonCall };
      llvm::SmallVector<InterruptedSite> Modes{InterruptedSite::Leaf,
                                               InterruptedSite::Call,
                                               InterruptedSite::Prefix};
      if (NonCall)
        Modes = {InterruptedSite::NonCall};
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      for (auto Mode : Modes) {
        const bool Protected = Mode != InterruptedSite::Leaf;
        SCOPED_TRACE(static_cast<unsigned>(Mode));
        SCOPED_TRACE(Installed);
        SCOPED_TRACE(Protected);
        const auto &Map = Maps[Protected ? 0 : 1];
        const auto *Public = Image.findSymbol(Protected ? kSignalScopeSymbol
                                                        : kSignalTickSymbol);
        ASSERT_NE(Public, nullptr);
        const auto Owner =
            Installed ? Patch->PatchedFunctions[Protected ? 0 : 1].OwnerVA
                      : Map.OriginalVA;
        const auto *Frame = Image.ExceptionMetadata.findFunction(Owner);
        ASSERT_NE(Frame, nullptr);
        auto Rows = recoverDarwinAArch64Unwind(Image, *Frame);
        ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
        std::optional<NDDarwinArm64eExceptionSite> Site;
        std::optional<AArch64SystemCallerPlan> Plan;
        for (auto PC = Owner; PC < Frame->CodeRange.End;
             PC += NDArm64eInstructionAlignment) {
          auto Candidate =
              prepareDarwinArm64eExceptionSite(Image, *Frame, *Rows, PC, 0);
          if (!Candidate || Candidate->AtProtectedCall != Protected ||
              bool(Candidate->ProtectedReturn) !=
                  (Mode == InterruptedSite::Prefix))
            continue;
          const auto *Code = Image.readVA(PC, NDArm64eInstructionAlignment);
          ASSERT_NE(Code, nullptr);
          DecodedInsn Instruction{};
          ASSERT_EQ(Decode.decodeOne(Code, NDArm64eInstructionAlignment, PC,
                                     Instruction),
                    NDArm64eInstructionAlignment);
          if (Mode == InterruptedSite::Call &&
              !cs_insn_group(Decode.getHandle(), Instruction.Raw, CS_GRP_CALL))
            continue;
          if (NonCall && Instruction.Id != AARCH64_INS_LDR)
            continue;
          Site = Candidate;
          Plan = prepareDarwinArm64eSystemCaller(
              aarch64UnwindRowAt(*Rows, PC - Owner), Frame->CodeRange, 0);
          if (PCSigning) {
            auto Loaded = prepareDarwinArm64eSystemCaller(
                aarch64UnwindRowAt(*Rows, PC - Owner), Frame->CodeRange, Owner);
            ASSERT_TRUE(Loaded);
            EXPECT_EQ(Loaded->Frame.SigningPC, Owner + kSignalSigningOffset);
          }
          break;
        }
        ASSERT_TRUE(Site && Plan);
        if (NonCall)
          EXPECT_EQ(Plan->Frame.SavedLR, !LiveLR);
        if (LiveLR)
          EXPECT_EQ(Plan->Frame.Signing, Signing);
        if (PCSigning) {
          EXPECT_EQ(Plan->Frame.SigningPC, kSignalSigningOffset);
          ++StructuralImages;
        }
        std::ofstream Out(Rules, std::ios::binary);
        const auto Write = [&](const auto &Value) {
          Out.write(reinterpret_cast<const char *>(&Value), sizeof(Value));
        };
        Write(*Site);
        Write(Plan->Frame);
        Write(Plan->Vectors);
        Write(Plan->Count);
        for (size_t I = 0; I < Plan->Count; ++I)
          Write(Plan->Saved[I]);
        Out.close();
        ASSERT_TRUE(Out);
        if (Execute)
          for (bool Alternate : {false, true}) {
            SCOPED_TRACE(Alternate);
            std::string Error;
            ASSERT_EQ(llvm::sys::ExecuteAndWait(
                          Driver,
                          {Driver, Installed ? Output : Input, Rules,
                           std::to_string(Owner - Public->Addr),
                           std::to_string(Alternate),
                           std::to_string(Installed && !Lifted ? 3 : 1)},
                          std::nullopt, {}, kSignalDeliverySeconds, 0, &Error),
                      0)
                << Error;
            ++Delivered;
          }
      }
    }
    ASSERT_TRUE(std::filesystem::remove(Output));
  }
  ::testing::Test::RecordProperty(
      NonCall ? (ObjectiveC ? kSignalNonCallObjCExceptions
                            : kSignalNonCallExceptions)
              : (ObjectiveC ? kSignalObjCExceptions : kSignalExceptions),
      Delivered);
  if (PCSigning) {
    ::testing::Test::RecordProperty(kSignalPCImages, StructuralImages);
    ::testing::Test::RecordProperty(kSignalPCNative, Delivered);
  }
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64AsyncUnwind, SignalExceptionRunsInterruptedAndOuterCleanups) {
  verifySignalLanguageCleanup(false);
}
TEST(AArch64AsyncUnwind, SignalExceptionRunsInterruptedAndOuterObjCCleanups) {
  verifySignalLanguageCleanup(true);
}

TEST(AArch64AsyncUnwind, ProtectedNonCallRunsInterruptedAndOuterCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::IndexedNonCall);
}

TEST(AArch64AsyncUnwind, ProtectedNonCallRunsInterruptedAndOuterObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::IndexedNonCall);
}

TEST(AArch64AsyncUnwind, LiftedNonCallRunsInterruptedAndOuterCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::IndexedNonCall, true);
}

TEST(AArch64AsyncUnwind, LiftedNonCallRunsInterruptedAndOuterObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::IndexedNonCall, true);
}

TEST(AArch64AsyncUnwind, CompactNonCallRunsInterruptedAndOuterCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactNonCall);
}

TEST(AArch64AsyncUnwind, CompactNonCallRunsInterruptedAndOuterObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactNonCall);
}

TEST(AArch64AsyncUnwind, LiftedCompactNonCallRunsInterruptedAndOuterCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactNonCall, true);
}

TEST(AArch64AsyncUnwind,
     LiftedCompactNonCallRunsInterruptedAndOuterObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactNonCall, true);
}

TEST(AArch64AsyncUnwind, NativeLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall);
}
TEST(AArch64AsyncUnwind, NativeLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall);
}
TEST(AArch64AsyncUnwind, LiftedLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, true);
}
TEST(AArch64AsyncUnwind, LiftedLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, true);
}

TEST(AArch64AsyncUnwind, NativeLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch);
}
TEST(AArch64AsyncUnwind, NativeLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch);
}
TEST(AArch64AsyncUnwind, LiftedLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, true);
}
TEST(AArch64AsyncUnwind, LiftedLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, true);
}

TEST(AArch64AsyncUnwind, NativeCompactLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, false);
}

TEST(AArch64AsyncUnwind, NativeCompactLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, false);
}

TEST(AArch64AsyncUnwind, NativeCompactLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, false);
}

TEST(AArch64AsyncUnwind, NativeCompactLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, false);
}

TEST(AArch64AsyncUnwind, LiftedCompactLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, true);
}

TEST(AArch64AsyncUnwind, LiftedCompactLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, true);
}

TEST(AArch64AsyncUnwind, LiftedCompactLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, true);
}

TEST(AArch64AsyncUnwind, LiftedCompactLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, true);
}

TEST(AArch64AsyncUnwind, NativeUnsignedLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, NativeUnsignedLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, false,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, NativeUnsignedLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, NativeUnsignedLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, false,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, LiftedUnsignedLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, LiftedUnsignedLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, true,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, LiftedUnsignedLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, LiftedUnsignedLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, true,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, NativeIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, NativeIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, false,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, NativeIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, NativeIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, false,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, LiftedIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, LiftedIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, true,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, LiftedIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, LiftedIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, true,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, NativeCompactUnsignedLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, false,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, NativeCompactUnsignedLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, NativeCompactUnsignedLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, false,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, NativeCompactUnsignedLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, LiftedCompactUnsignedLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, true,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, LiftedCompactUnsignedLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, LiftedCompactUnsignedLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, true,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, LiftedCompactUnsignedLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eUnsigned);
}

TEST(AArch64AsyncUnwind, NativeCompactIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, false,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, NativeCompactIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, NativeCompactIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, false,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, NativeCompactIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, LiftedCompactIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, true,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, LiftedCompactIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, LiftedCompactIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, true,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, LiftedCompactIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eSignedIAWithCFA);
}

TEST(AArch64AsyncUnwind, NativePACMIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativePACMIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, false,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativePACMIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativePACMIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, false,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedPACMIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedPACMIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, true,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedPACMIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedPACMIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, true,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativeCompactPACMIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, false,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativeCompactPACMIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativeCompactPACMIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, false,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativeCompactPACMIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedCompactPACMIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, true,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedCompactPACMIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedCompactPACMIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, true,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedCompactPACMIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eSignedIAWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativePACMIBLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativePACMIBLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, false,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativePACMIBLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativePACMIBLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, false,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedPACMIBLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedPACMIBLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, true,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedPACMIBLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedPACMIBLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, true,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativeCompactPACMIBLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, false,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativeCompactPACMIBLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativeCompactPACMIBLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, false,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativeCompactPACMIBLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedCompactPACMIBLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, true,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedCompactPACMIBLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedCompactPACMIBLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, true,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, LiftedCompactPACMIBLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eSignedIBWithCFAPACM);
}

TEST(AArch64AsyncUnwind, NativeExplicitIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeExplicitIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, false,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeExplicitIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeExplicitIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, false,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedExplicitIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedExplicitIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, true,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedExplicitIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedExplicitIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, true,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeCompactExplicitIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, false,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind,
     NativeCompactExplicitIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeCompactExplicitIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, false,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeCompactExplicitIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedCompactExplicitIALiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, true,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind,
     LiftedCompactExplicitIALiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedCompactExplicitIALiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, true,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedCompactExplicitIALiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eSignedIAWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeExplicitIBLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeExplicitIBLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, false,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeExplicitIBLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, false,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeExplicitIBLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, false,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedExplicitIBLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedExplicitIBLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::LiveLRCatch, true,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedExplicitIBLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRNonCall, true,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedExplicitIBLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::LiveLRCatch, true,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeCompactExplicitIBLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, false,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind,
     NativeCompactExplicitIBLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeCompactExplicitIBLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, false,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeCompactExplicitIBLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, false,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedCompactExplicitIBLiveLRRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLR, true,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind,
     LiftedCompactExplicitIBLiveLRCatchesLanguageException) {
  verifySignalLanguageCleanup(false, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedCompactExplicitIBLiveLRRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLR, true,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, LiftedCompactExplicitIBLiveLRCatchesObjCException) {
  verifySignalLanguageCleanup(true, LanguageFrame::CompactLiveLRCatch, true,
                              NDArm64eSignedIBWithCFAPC);
}

TEST(AArch64AsyncUnwind, NativeFramelessRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::FramelessNonCall);
}
TEST(AArch64AsyncUnwind, NativeFramelessRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::FramelessNonCall);
}
TEST(AArch64AsyncUnwind, LiftedFramelessRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::FramelessNonCall, true);
}
TEST(AArch64AsyncUnwind, LiftedFramelessRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::FramelessNonCall, true);
}
TEST(AArch64AsyncUnwind, NativeExplicitLPStartPreservesTypedActions) {
  verifySignalLanguageCleanup(false, LanguageFrame::TypedExplicitLPStart);
}
TEST(AArch64AsyncUnwind, LiftedExplicitLPStartPreservesTypedActions) {
  verifySignalLanguageCleanup(false, LanguageFrame::TypedExplicitLPStart, true);
}

TEST(AArch64AsyncUnwind, PublishesExplicitLPStartWithEntryLandingPad) {
  verifySignalLanguageCleanup(false, LanguageFrame::EntryLandingPad, true);
}

TEST(AArch64AsyncUnwind, NativeExplicitLPStartRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::ExplicitLPStart);
}

TEST(AArch64AsyncUnwind, NativeExplicitLPStartRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::ExplicitLPStart);
}

TEST(AArch64AsyncUnwind, LiftedExplicitLPStartRunsLanguageCleanups) {
  verifySignalLanguageCleanup(false, LanguageFrame::ExplicitLPStart, true);
}

TEST(AArch64AsyncUnwind, LiftedExplicitLPStartRunsObjCCleanups) {
  verifySignalLanguageCleanup(true, LanguageFrame::ExplicitLPStart, true);
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
TEST(AArch64AsyncUnwind, ObjectiveCCatchesAndFinallyPreserveSignedCallers) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kDirectory, Dir));
  const std::filesystem::path Root(Dir.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kObjCFile).string();
  const auto ProviderSource = (Root / kObjCProviderFile).string();
  const auto DriverSource = (Root / kObjCDriverFile).string();
  const auto Provider = (Root / kCatchImports).string();
  const auto Object = (Root / kObject).string();
  const auto Output = (Root / kPublished).string();
  const auto Driver = (Root / kDriver).string();
  std::ofstream(Source) << kObjCBody;
  std::ofstream(ProviderSource) << kObjCProvider;
  std::ofstream(DriverSource) << kObjCDriver;

  unsigned Paths = 0;
  auto Reader = Loader::create(BinaryFormat::MachO);
  for (const auto *Opt : {kNoOptimize, kOptimize}) {
    for (bool Weak : {false, true}) {
      SCOPED_TRACE(Opt);
      SCOPED_TRACE(Weak);
      const auto Input =
          (Root / (Weak ? kObjCWeakImage : kObjCStrongImage)).string();
      const auto Compile = [&](llvm::ArrayRef<llvm::StringRef> Flags) {
        llvm::SmallVector<llvm::StringRef> Args{kCompiler,     kTargetFlag,
                                                kTarget,       Opt,
                                                kFramePointer, kObjCExceptions};
        if (Weak)
          Args.push_back(kObjCWeakClass);
        Args.append(Flags.begin(), Flags.end());
        return llvm::sys::ExecuteAndWait(kCompiler, Args);
      };
      ASSERT_EQ(Compile({kDylib, ProviderSource, kObjCFramework, kFoundation,
                         kOutput, Provider}),
                0);
      ASSERT_EQ(Compile({kDylib, Source, Provider, kObjCFramework, kFoundation,
                         kOutput, Input}),
                0);
      ASSERT_EQ(Compile({kCatchReplacement, kCompile, Source, kOutput, Object}),
                0);
      auto Original = Reader->load(Input);
      ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
      const auto *Entry = Original->findSymbol(kCatchSymbol);
      ASSERT_NE(Entry, nullptr);
      const auto *Frame = Original->ExceptionMetadata.findFunction(Entry->Addr);
      ASSERT_NE(Frame, nullptr);
      const bool Dwarf = Frame->Dwarf.has_value();
      va_t Slot = 0;
      ASSERT_NO_FATAL_FAILURE(
          checkLanguageLandingPads(*Original, Entry->Addr, Dwarf, false, Slot,
                                   ExceptionPersonality::ObjCPersonalityV0));
      const auto Bytes = llvm::MemoryBuffer::getFile(Object);
      ASSERT_TRUE(bool(Bytes));
      const SourcePatchMapping Map{kCatchMapping, Entry->Addr};
      auto Patch = installMachOObjectPatch((*Bytes)->getBuffer(), *Original,
                                           {Map}, {}, Output, {});
      ASSERT_TRUE(bool(Patch)) << llvm::toString(Patch.takeError());
      ASSERT_EQ(Patch->PatchedFunctions.size(), 1u);
      const auto Owner = Patch->PatchedFunctions.front().OwnerVA;
      ASSERT_NE(Owner, Entry->Addr);
      auto Published = Reader->load(Output);
      ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());
      va_t NewSlot = 0;
      ASSERT_NO_FATAL_FAILURE(
          checkLanguageLandingPads(*Published, Owner, Dwarf, false, NewSlot,
                                   ExceptionPersonality::ObjCPersonalityV0));
      EXPECT_EQ(NewSlot, Slot + Patch->OriginalSectionSlide);
      if (std::getenv(kExecute)) {
        ASSERT_EQ(Compile({DriverSource, Provider, kObjCFramework, kFoundation,
                           kOutput, Driver}),
                  0);
        ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, Input}), 0);
        ASSERT_EQ(
            llvm::sys::ExecuteAndWait(Driver, {Driver, Output, kPublished}), 0);
        Paths += 2 * kExceptionInputs;
      }
      ASSERT_TRUE(std::filesystem::remove(Output));
      const auto Reject = [&](llvm::StringRef Contents,
                              const BinaryImage &Image, va_t Address) {
        SourcePatchMapping Mapping{kCatchMapping, Address};
        auto Rejected =
            installMachOObjectPatch(Contents, Image, {Mapping}, {}, Output, {});
        ASSERT_FALSE(bool(Rejected));
        llvm::consumeError(Rejected.takeError());
        EXPECT_FALSE(std::filesystem::exists(Output));
      };
      if (Weak) {
        auto Strong = Reader->load((Root / kObjCStrongImage).string());
        ASSERT_TRUE(bool(Strong)) << llvm::toString(Strong.takeError());
        const auto *Other = Strong->findSymbol(kCatchSymbol);
        ASSERT_NE(Other, nullptr);
        ASSERT_NO_FATAL_FAILURE(
            Reject((*Bytes)->getBuffer(), *Strong, Other->Addr));
        ASSERT_EQ(Compile({kObjCDirectRef, kCompile, Source, kOutput, Object}),
                  0);
        auto Direct = llvm::MemoryBuffer::getFile(Object);
        ASSERT_TRUE(bool(Direct));
        ASSERT_NO_FATAL_FAILURE(
            Reject((*Direct)->getBuffer(), *Original, Entry->Addr));
      } else {
        std::string Changed = (*Bytes)->getBuffer().str();
        auto File = llvm::object::ObjectFile::createObjectFile(
            llvm::MemoryBufferRef(Changed, {}));
        ASSERT_TRUE(bool(File)) << llvm::toString(File.takeError());
        bool Found = false;
        for (const auto &Section : (*File)->sections()) {
          auto Name = Section.getName();
          ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
          if (*Name != kObjCFlagsSection)
            continue;
          auto Data = Section.getContents();
          ASSERT_TRUE(bool(Data)) << llvm::toString(Data.takeError());
          ASSERT_FALSE(Data->empty());
          Changed[Data->data() - Changed.data()] ^= 1;
          Found = true;
        }
        ASSERT_TRUE(Found);
        ASSERT_NO_FATAL_FAILURE(Reject(Changed, *Original, Entry->Addr));
      }
    }
  }
  RecordProperty(kObjCPaths, Paths);
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

} // namespace
