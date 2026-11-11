//===- AArch64AsyncUnwindTests.cpp - Interrupted native return signing ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/ReturnAddressSigning.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
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
      ASSERT_TRUE(Row.hasAddress());
      ASSERT_GE(Row.getAddress(), Entry);
      if (Rows.empty() || Rows.back().Depth != CFA.getOffset() ||
          Rows.back().SavedLR != Saved)
        Rows.push_back({Row.getAddress() - Entry, CFA.getOffset(), Saved});
    }
  }
  ASSERT_FALSE(Rows.empty());
  ASSERT_EQ(Rows.front().Offset, 0u);
}

void checkFrame(const BinaryImage &Image, va_t Entry,
                const std::filesystem::path &Samples,
                llvm::ArrayRef<StackRow> Rows) {
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
  bool Signed = false;
  unsigned Signs = 0, Auths = 0;
  for (uint64_t Off = 0; Off < FDE.AddressRange; Off += aarch64::kInsnSize) {
    ASSERT_GE(FDE.AddressRange - Off, aarch64::kInsnSize);
    const auto At = queryAArch64UnwindSigning(Image, *Frame, Entry + Off);
    ASSERT_TRUE(At.DwarfStateAtPC && At.DwarfStateAtPC->State);
    const auto &State = *At.DwarfStateAtPC;
    EXPECT_EQ(State.State->Kind,
              Signed ? dwarf_eh::AArch64RASigningKind::SP
                     : dwarf_eh::AArch64RASigningKind::Unsigned);
    if (Signed)
      EXPECT_EQ(State.State->Key, dwarf_eh::AArch64RAKey::B);
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
    const auto At = queryAArch64UnwindSigning(Image, *Frame, Entry + Off);
    ASSERT_TRUE(At.DwarfStateAtPC && At.DwarfStateAtPC->State);
    const auto &State = *At.DwarfStateAtPC;
    EXPECT_EQ(State.State->Kind,
              IsSigned ? dwarf_eh::AArch64RASigningKind::SP
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
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, {kCompiler, kTargetFlag,
                                                  kTarget, kOptimize, Observer,
                                                  Wrapper, kOutput, Driver}),
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
  for (uint64_t Off = 0; Off < Size; Off += aarch64::kInsnSize) {
    const auto Implicit =
        queryAArch64UnwindSigning(*Published, *NewFrame, Owner + Off);
    ASSERT_TRUE(Implicit.DwarfStateAtPC);
    EXPECT_EQ(Implicit.DwarfStateAtPC->Status,
              dwarf_eh::AArch64RAQueryStatus::Unknown);
    EXPECT_FALSE(Implicit.DwarfStateAtPC->State);
  }
  if (std::getenv(kExecute)) {
    const auto OriginalSamples = (Root / kOriginalSamples).string();
    const auto InstalledSamples = (Root / kInstalledSamples).string();
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  Driver, {Driver, Input, std::to_string(0),
                           std::to_string(kOldValue), OriginalSamples}),
              0);
    ASSERT_NO_FATAL_FAILURE(
        checkFrame(*Original, Entry, OriginalSamples, OriginalRows));
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  Driver, {Driver, Output, std::to_string(Owner - Entry),
                           std::to_string(kNewValue), InstalledSamples}),
              0);
    // Both bodies have identical machine instructions at the same offsets.
    // Only the original describes the signing transitions in its DWARF rows.
    ASSERT_NO_FATAL_FAILURE(
        checkFrame(*Original, Entry, InstalledSamples, PublishedRows));
    for (auto [Path, Property] :
         {std::pair(OriginalSamples, kOriginalRows),
          std::pair(InstalledSamples, kPublishedRows)}) {
      auto Rows = llvm::MemoryBuffer::getFile(Path);
      ASSERT_TRUE(bool(Rows));
      RecordProperty(Property, (*Rows)->getBuffer().str());
    }
    RecordProperty(kSamples, 2 * kStages);
  }
#else
  GTEST_SKIP() << kUnavailable;
#endif
}
} // namespace
