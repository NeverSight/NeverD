//===- AArch64SavedRegisterTests.cpp - Lifted frame registers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Publish whole lifted frames with every Darwin callee-saved register, then
/// compare normal returns and signal-driven signed caller resumption.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/AArch64UnwindRecovery.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/SectionNames.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
struct CompactPairCase {
  unsigned Mask;
  unsigned First;
  bool Vector;
};
#define SAVED_TEXT(Name, Text) constexpr char Name[] = Text;
#define SAVED_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64SavedRegisterFixtures.def"
#define SAVED_TEXT(Name, Text) constexpr char Name[] = Text;
#define SAVED_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64CompactRegisterFixtures.def"

#if defined(__APPLE__) && defined(__aarch64__)
// This is a final-image format fixture, not a test of Apple's selection of
// compact object records. Xcode 27 rewrites the nonzero-offset input record
// into ordinary FP mode. Install the independently specified fixture word in
// the validated encoding slots before signing the test image.
void writeCompactFixture(const std::string &Path, const BinaryImage &Image,
                         uint32_t Encoding, bool BothFunctions = false) {
  const auto *Section = Image.getSectionByName(section_names::macho::Unwind);
  const auto *Symbol = Image.findSymbol(kSumSymbol);
  ASSERT_TRUE(Section && Symbol);
  auto Raw = macho_unwind::parseCompactUnwindRaw(
      llvm::ArrayRef(Image.Raw).slice(Section->FileOff, Section->Size));
  ASSERT_TRUE(bool(Raw)) << llvm::toString(Raw.takeError());
  ASSERT_EQ(Raw->Pages.size(), 1u);
  const auto &Page = Raw->Pages.front();
  llvm::SmallVector<va_t, 2> Starts{Symbol->Addr};
  if (BothFunctions) {
    const auto *Second = Image.findSymbol(kDifferenceSymbol);
    ASSERT_NE(Second, nullptr);
    Starts.push_back(Second->Addr);
  }
  auto Bytes = Image.Raw;
  for (va_t Start : Starts) {
    const auto Owns = [&](const auto &Entry) {
      return Entry.FunctionOffset + Image.Base == Start;
    };
    size_t Offset = 0;
    if (Page.Kind == macho_unwind::kSecondLevelRegular) {
      const auto Entry = llvm::find_if(Page.RegularEntries, Owns);
      ASSERT_NE(Entry, Page.RegularEntries.end());
      Offset = Page.SectionOffset + Page.EntryPageOffset +
               (Entry - Page.RegularEntries.begin()) *
                   sizeof(macho_unwind::CompactUnwindRawRegularEntry) +
               sizeof(uint32_t);
    } else {
      ASSERT_EQ(Page.Kind, macho_unwind::kSecondLevelCompressed);
      const auto Entry = llvm::find_if(Page.CompressedEntries, Owns);
      ASSERT_NE(Entry, Page.CompressedEntries.end());
      const auto Index = Entry->EncodingIndex;
      ASSERT_TRUE(llvm::all_of(Page.CompressedEntries, [&](const auto &E) {
        return E.EncodingIndex != Index ||
               llvm::is_contained(Starts, Image.Base + E.FunctionOffset);
      }));
      Offset =
          Index < Raw->CommonEncodings.size()
              ? Raw->Header.CommonEncodingsSectionOffset +
                    Index * sizeof(uint32_t)
              : Page.SectionOffset + Page.EncodingsPageOffset +
                    (Index - Raw->CommonEncodings.size()) * sizeof(uint32_t);
    }
    writeLE<uint32_t>(Bytes.data() + Section->FileOff + Offset, Encoding);
  }
  std::ofstream File(Path, std::ios::binary | std::ios::trunc);
  File.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  File.close();
  ASSERT_TRUE(File);
}
#endif

void verifySavedRegisters(bool Quad, bool Signal,
                          std::optional<unsigned> CompactPairs = std::nullopt,
                          bool ExtraStack = false, bool Coalesced = false,
                          bool Frameless = false,
                          std::optional<unsigned> SigningOffset = std::nullopt,
                          bool Language = false, bool IndexedPeer = false) {
#if defined(__APPLE__) && defined(__aarch64__)
  const bool Compact = CompactPairs.has_value();
  const unsigned Functions = Compact && !Coalesced ? 1 : kFunctions;
  const char *Names[] = {kSum, kDifference};
  const auto FunctionNames = llvm::ArrayRef(Names).take_front(Functions);
  llvm::SmallVector<std::pair<unsigned, int>> CompactSpills;
  unsigned VectorMask = Compact ? 0 : kVectorMask;
  int SpillOffset = Frameless ? kFramelessFirstSpill : kCompactFirstSpill;
  if (Compact)
    for (const auto &Pair : kCompactPairCases) {
      if (!(*CompactPairs & Pair.Mask))
        continue;
      for (unsigned I = 0; I < kCompactPairRegisters; ++I) {
        CompactSpills.emplace_back(
            Pair.First + I + (Pair.Vector ? aarch64_unwind::kDwarfV0 : 0),
            SpillOffset);
        SpillOffset -= sizeof(uint64_t);
        if (Pair.Vector)
          VectorMask |= 1u << (Pair.First + I - NDArm64eFirstPreservedD);
      }
    }
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kDirectory, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kSource).string();
  const auto Wrapper = (Root / kWrapper).string();
  const auto Caller = (Root / kCaller).string();
  const auto Driver = (Root / kDriver).string();
  const auto Input = (Root / kOriginal).string();
  const auto Output = (Root / kPublished).string();
  const auto Rejected = (Root / kRejected).string();
  const auto RulesPath = (Root / kRules).string();
  {
    std::ofstream Assembly(Source);
    Assembly << (Compact ? kCompactBody : kBody);
    if (IndexedPeer)
      Assembly << kIndexedPeerBody;
  }
  std::ofstream(Wrapper) << kObserver;
  std::ofstream(Caller) << kDriverBody;
  llvm::SmallVector<llvm::StringRef> Common{kCompiler, kTargetFlag, kTarget,
                                            kOptimize};
  if (Quad)
    Common.push_back(kQuadFlag);
  if (Signal)
    Common.push_back(kSignalFlag);
  const auto CompactFlag =
      std::string(kCompactFlag) + std::to_string(CompactPairs.value_or(0));
  if (Compact)
    Common.push_back(CompactFlag);
  if (ExtraStack)
    Common.push_back(kExtraStackFlag);
  if (Coalesced)
    Common.push_back(kCoalescedFlag);
  if (Frameless)
    Common.push_back(kFramelessFlag);
  const auto PAuthFlag =
      std::string(kPAuthLRFlag) + std::to_string(SigningOffset.value_or(0));
  if (SigningOffset)
    Common.push_back(PAuthFlag);
  if (Language)
    Common.push_back(kLanguageFlag);
  auto Compile = Common;
  if (Language || IndexedPeer)
    Compile.push_back(kCXXRuntime);
  Compile.append({kDylib, kTight, Source, kOutputFlag, Input});
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, Compile), 0);
  const auto *Override = std::getenv(kHeaderRoot);
  const auto Include =
      Override ? std::string(Override)
               : (std::filesystem::path(__FILE__).parent_path() / kIncludes)
                     .lexically_normal()
                     .string();
  Compile = Common;
  Compile.append({kIncludeFlag, Include, kWarnings, kExtraWarnings,
                  kErrorWarnings, Caller, Wrapper, kOutputFlag, Driver});
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, Compile), 0);
  if (SigningOffset)
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  kFixtureCodesign,
                  {kFixtureCodesign, kFixtureRemoveSignature, Input}),
              0);

  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Image = Reader->load(Input);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  if (SigningOffset) {
    const auto *OriginalSymbol = Image->findSymbol(kSumSymbol);
    ASSERT_NE(OriginalSymbol, nullptr);
    const auto *OriginalFrame =
        Image->ExceptionMetadata.findFunction(OriginalSymbol->Addr);
    ASSERT_TRUE(OriginalFrame && OriginalFrame->Compact);
    testing::Test::RecordProperty(kLinkerCompactWord,
                                  OriginalFrame->Compact->NativeEncoding);
    const uint32_t LanguageBits =
        OriginalFrame->Compact->NativeEncoding &
        (macho_unwind::kPersonalityMask | macho_unwind::kHasLSDA);
    const uint32_t Encoding =
        kCompactPAuthLRMode | *CompactPairs | LanguageBits |
        ((*SigningOffset / aarch64::kInsnSize) << kPAuthLRShift);
    // Both an ordinary FP recipe and a PC field naming the next instruction
    // disagree with the actual PACIBSPPC site.
    for (const auto Wrong : {kCompactFrameMode | *CompactPairs | LanguageBits,
                             Encoding + (1u << kPAuthLRShift)}) {
      ASSERT_NO_FATAL_FAILURE(writeCompactFixture(Input, *Image, Wrong));
      auto Contradictory = Reader->load(Input);
      ASSERT_TRUE(bool(Contradictory))
          << llvm::toString(Contradictory.takeError());
      llvm::LLVMContext BadContext;
      PipelineOptions BadOptions;
      BadOptions.PatchMode = true;
      BadOptions.EmitDumpOutput = false;
      auto Bad = Pipeline().run(*Contradictory, BadContext, BadOptions);
      if (Bad.Success) {
        ASSERT_TRUE(Bad.LlvmModule);
        MachOPatcher BadPatcher;
        BadPatcher.setImageContext(&*Contradictory);
        EXPECT_FALSE(
            BadPatcher
                .patch(Input, Rejected, *Bad.LlvmModule, Contradictory->Arch)
                .Success);
      } else {
        EXPECT_FALSE(Bad.Error.empty());
      }
      EXPECT_FALSE(std::filesystem::exists(Rejected));
    }
    ASSERT_NO_FATAL_FAILURE(writeCompactFixture(Input, *Image, Encoding));
    Image = Reader->load(Input);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    const auto *Symbol = Image->findSymbol(kSumSymbol);
    ASSERT_NE(Symbol, nullptr);
    const auto *Frame = Image->ExceptionMetadata.findFunction(Symbol->Addr);
    ASSERT_TRUE(Frame && Frame->Compact);
    ASSERT_EQ(Frame->CodeRange.Begin, Symbol->Addr);
    ASSERT_EQ(Frame->Compact->NativeEncoding, Encoding);
  }
  bool Execute = std::getenv(kExecute);
  if (Execute && SigningOffset) {
    const int Probe =
        llvm::sys::ExecuteAndWait(Driver, {Driver, kPAuthLRProbe});
    ASSERT_TRUE(Probe == 0 || Probe == kPAuthLRUnavailableCode);
    Execute = Probe == 0;
  }
  const auto Snapshot = Image->Raw;
  if (Coalesced) {
    const auto *First = Image->findSymbol(kSumSymbol);
    const auto *Second = Image->findSymbol(kDifferenceSymbol);
    ASSERT_TRUE(First && Second);
    const auto *Frame = Image->ExceptionMetadata.findFunction(First->Addr);
    ASSERT_NE(Frame, nullptr);
    ASSERT_EQ(Frame, Image->ExceptionMetadata.findFunction(Second->Addr));
    ASSERT_LT(First->Addr, Second->Addr);
  }
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.PatchMode = true;
  Options.EmitDumpOutput = false;
  auto Lifted = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Lifted.Success) << Lifted.Error;
  ASSERT_TRUE(Lifted.LlvmModule);
  auto &Module = *Lifted.LlvmModule;
  symbolizeImageAbsolutePointers(Module, *Image);
  for (const auto *Name : FunctionNames) {
    const auto *Function = Module.getFunction(Name);
    ASSERT_NE(Function, nullptr);
    ASSERT_TRUE(Function->hasFnAttribute(llvm::Attribute::Naked));
  }

  MachOPatcher Patcher;
  Patcher.setImageContext(&*Image);
  // The matching instructions alone do not authorize changed unwind locations.
  llvm::SmallVector<std::pair<std::string, std::string>> Mutations;
  if (Compact) {
    auto Slots = CompactSpills;
    if (!Frameless)
      Slots.emplace_back(aarch64_unwind::kFP, kFPOffset);
    for (const auto &[Reg, Offset] : Slots) {
      const auto Prefix =
          std::string(kCFISpill) + std::to_string(Reg) + kCFISeparator;
      Mutations.emplace_back(
          Prefix + std::to_string(Offset),
          Prefix + std::to_string(Offset + int(sizeof(uint64_t))));
    }
  } else {
    Mutations = {{kCFIX19, kWrongCFIX19},
                 {Quad ? kCFIQ8 : kCFID8, Quad ? kWrongCFIQ8 : kWrongCFID8}};
  }
  if (SigningOffset)
    Mutations.emplace_back(kPAuthLRCFI, kMissingPAuthLRCFI);
  for (const auto &[Before, After] : Mutations) {
    auto Changed = llvm::CloneModule(Module);
    auto *Function = Changed->getFunction(kSum);
    auto *Call = llvm::dyn_cast<llvm::CallInst>(&Function->front().front());
    ASSERT_NE(Call, nullptr);
    auto *Assembly = llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
    ASSERT_NE(Assembly, nullptr);
    std::string Text = Assembly->getAsmString().str();
    const auto At = Text.find(Before);
    ASSERT_NE(At, std::string::npos) << Before << '\n' << Text;
    Text.replace(At, llvm::StringRef(Before).size(), After);
    Call->setCalledOperand(llvm::InlineAsm::get(
        Assembly->getFunctionType(), Text, Assembly->getConstraintString(),
        Assembly->hasSideEffects(), Assembly->isAlignStack(),
        Assembly->getDialect(), Assembly->canThrow()));
    EXPECT_FALSE(Patcher.patch(Input, Rejected, *Changed, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }

  auto Installed = Patcher.patch(Input, Output, Module, Image->Arch);
  ASSERT_TRUE(Installed.Success);
  ASSERT_EQ(Installed.PatchedFunctions.size(),
            Functions + unsigned(IndexedPeer));
  auto Published = Reader->load(Output);
  ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());
  EXPECT_TRUE(Published->isArm64e());
  EXPECT_NE(Published->Raw, Snapshot);
  if (SigningOffset && !IndexedPeer &&
      !Image->getSectionByName(section_names::macho::EhFrame))
    EXPECT_FALSE(Published->getSectionByName(section_names::macho::EhFrame));
  for (const auto &Entry : Installed.PatchedFunctions) {
    EXPECT_NE(Entry.OwnerVA, Entry.OriginalVA);
    const auto *Before =
        Image->ExceptionMetadata.findFunction(Entry.OriginalVA);
    const auto *After =
        Published->ExceptionMetadata.findFunction(Entry.OwnerVA);
    ASSERT_TRUE(Before && After);
    if (IndexedPeer && Entry.SourceFunction == kIndexedPeer) {
      ASSERT_TRUE(Before->Dwarf && After->Dwarf && After->Compact);
      ASSERT_TRUE(Before->Itanium && After->Itanium);
      EXPECT_EQ(After->Compact->NativeEncoding & macho_unwind::kModeMask,
                macho_unwind::kARM64ModeDwarf);
      EXPECT_EQ(After->Compact->NativeEncoding &
                    macho_unwind::kDwarfSectionOffsetMask,
                After->Dwarf->SectionOffset);
      EXPECT_EQ(After->Dwarf->InitialLocation, Entry.OwnerVA);
      EXPECT_EQ(After->Personality, Before->Personality);
      EXPECT_EQ(After->Dwarf->LSDAVA, After->Itanium->LSDAVA);
      ASSERT_EQ(Before->Itanium->CallSites.size(), 1u);
      ASSERT_EQ(After->Itanium->CallSites.size(), 1u);
      EXPECT_EQ(After->Itanium->CallSites.front().LandingPadVA - Entry.OwnerVA,
                Before->Itanium->CallSites.front().LandingPadVA -
                    Entry.OriginalVA);
      continue;
    }
    if (Compact) {
      ASSERT_TRUE(Before->Compact && After->Compact);
      EXPECT_FALSE(Before->Dwarf);
      if (Language) {
        ASSERT_TRUE(Before->Itanium && After->Itanium);
        EXPECT_EQ(Before->Personality, After->Personality);
        EXPECT_EQ(Before->Itanium->LSDAVA + Installed.OriginalSectionSlide,
                  After->Itanium->LSDAVA);
        ASSERT_EQ(After->Itanium->CallSites.size(), 1u);
        EXPECT_EQ(After->Itanium->CallSites.front().LandingPadVA -
                      After->CodeRange.Begin,
                  Before->Itanium->CallSites.front().LandingPadVA -
                      Before->CodeRange.Begin);
      }
      if (Frameless && !CompactSpills.empty()) {
        ASSERT_TRUE(After->Dwarf);
        EXPECT_EQ(After->Compact->NativeEncoding & macho_unwind::kModeMask,
                  macho_unwind::kARM64ModeDwarf);
        std::vector<CFIInstruction> Expected{
            {.Kind = CFIOpKind::GnuArgsSize},
            {.Kind = CFIOpKind::DefCFAOffset,
             .Offset =
                 kCompactFrameDepth + (ExtraStack ? kCompactExtraDepth : 0)}};
        for (const auto &[Reg, Offset] : CompactSpills)
          Expected.push_back(
              {.Kind = CFIOpKind::Offset, .Register = Reg, .Offset = Offset});
        const auto Effective = [](const auto &Row) {
          return Row.Kind != CFIOpKind::Nop;
        };
        EXPECT_TRUE(llvm::equal(
            Expected,
            llvm::make_filter_range(After->Dwarf->Instructions, Effective),
            dwarf_eh::sameCFIInstruction));
      } else {
        EXPECT_FALSE(After->Dwarf);
        EXPECT_EQ(After->Compact->NativeEncoding,
                  Before->Compact->NativeEncoding);
      }
      const auto Mode =
          Frameless ? kCompactFramelessMode |
                          ((kCompactFrameDepth +
                            (ExtraStack ? kCompactExtraDepth : 0)) /
                           kCompactStackScale)
                              << kCompactStackShift
          : SigningOffset
              ? kCompactPAuthLRMode | (*SigningOffset / aarch64::kInsnSize)
                                          << kPAuthLRShift
              : kCompactFrameMode;
      EXPECT_EQ(Before->Compact->NativeEncoding &
                    ~(macho_unwind::kPersonalityMask | macho_unwind::kHasLSDA),
                Mode | *CompactPairs);
      continue;
    }
    ASSERT_TRUE(Before->Dwarf && After->Dwarf);
    const auto *CIE =
        Published->ExceptionMetadata.findCIE(After->Dwarf->CIESectionOffset);
    ASSERT_TRUE(CIE && CIE->HasPointerAuth);
    const auto Effective = [](const auto &Row) {
      return Row.Kind != CFIOpKind::Nop;
    };
    EXPECT_TRUE(llvm::equal(
        llvm::make_filter_range(Before->Dwarf->Instructions, Effective),
        llvm::make_filter_range(After->Dwarf->Instructions, Effective),
        dwarf_eh::sameCFIInstruction));
    for (unsigned Reg = NDArm64eFirstPreservedGPR;
         Reg < aarch64_unwind::kFrameRegisters; ++Reg) {
      const bool GPR = Reg <= aarch64_unwind::kLR;
      const bool Vector = Reg >= aarch64_unwind::kFirstDSlot &&
                          Reg < aarch64_unwind::kFirstHighSlot;
      if (!GPR && !Vector)
        continue;
      const unsigned Dwarf = GPR ? Reg
                                 : aarch64_unwind::kDwarfV0 +
                                       NDArm64eFirstPreservedD + Reg -
                                       aarch64_unwind::kFirstDSlot;
      for (auto Kind :
           {CFIOpKind::Offset, CFIOpKind::Restore, CFIOpKind::SameValue})
        EXPECT_TRUE(
            llvm::any_of(After->Dwarf->Instructions, [&](const auto &Row) {
              return Row.Kind == Kind && Row.Register == Dwarf;
            }));
    }
  }

  unsigned Executions = 0;
  for (bool InstalledImage : {false, true}) {
    SCOPED_TRACE(InstalledImage);
    auto ExecutionPath = InstalledImage ? Output : Input;
    if (SigningOffset) {
      const auto SignedPath =
          (Root / (kSignedFixturePrefix +
                   std::filesystem::path(ExecutionPath).filename().string()))
              .string();
      ASSERT_TRUE(std::filesystem::copy_file(ExecutionPath, SignedPath));
      ASSERT_EQ(llvm::sys::ExecuteAndWait(
                    kFixtureCodesign, {kFixtureCodesign, kFixtureAdHocSignature,
                                       kFixtureAdHocIdentity, SignedPath}),
                0);
      ASSERT_EQ(
          llvm::sys::ExecuteAndWait(kFixtureCodesign,
                                    {kFixtureCodesign, kFixtureVerifySignature,
                                     kFixtureStrictSignature, SignedPath}),
          0);
      ExecutionPath = SignedPath;
    }
    auto Reloaded = Reader->load(ExecutionPath);
    ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
    const auto &View = *Reloaded;
    testing::Test::RecordProperty(
        InstalledImage ? kPublishedHash : kOriginalHash,
        llvm::toHex(llvm::SHA256::hash(View.Raw), true));
    std::ofstream Rules(RulesPath);
    for (const auto &[Name, Symbol] :
         {std::pair{kSum, kSumSymbol},
          std::pair{kDifference, kDifferenceSymbol}}) {
      if (Compact && !Coalesced && Name == kDifference)
        continue;
      const auto Entry =
          llvm::find_if(Installed.PatchedFunctions, [&](const auto &E) {
            return E.SourceFunction == Name;
          });
      ASSERT_NE(Entry, Installed.PatchedFunctions.end());
      const auto VA = InstalledImage ? Entry->OwnerVA : Entry->OriginalVA;
      const auto *Public = View.findSymbol(Symbol);
      ASSERT_NE(Public, nullptr);
      const auto *Frame = View.ExceptionMetadata.findFunction(VA);
      ASSERT_NE(Frame, nullptr);
      auto Rows = recoverDarwinAArch64Unwind(View, *Frame);
      ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
      std::optional<uint64_t> Offset;
      const auto Wanted = Signal ? kWaitLoad : kPlanInstruction;
      auto End = Frame->CodeRange.End;
      for (const auto &Other : Installed.PatchedFunctions) {
        const auto Next = InstalledImage ? Other.OwnerVA : Other.OriginalVA;
        if (Next > VA)
          End = std::min(End, Next);
      }
      for (uint64_t At = 0; At < End - VA; At += aarch64::kInsnSize) {
        const auto *Code = View.readVA(VA + At, aarch64::kInsnSize);
        ASSERT_NE(Code, nullptr);
        if (readLE<uint32_t>(Code) == Wanted) {
          ASSERT_FALSE(Offset);
          Offset = At;
        }
      }
      ASSERT_TRUE(Offset);
      const auto &Row =
          aarch64UnwindRowAt(*Rows, VA + *Offset - Frame->CodeRange.Begin);
      const auto Plan = prepareDarwinArm64eSystemCaller(Row, Frame->CodeRange,
                                                        Frame->CodeRange.Begin);
      ASSERT_TRUE(Plan);
      ASSERT_TRUE(Plan->Vectors.Complete);
      EXPECT_EQ(Plan->Vectors.LowMask, VectorMask);
      EXPECT_EQ(Plan->Vectors.HighMask, Quad ? kVectorMask : 0u);
      EXPECT_EQ(Plan->Frame.Signing, SigningOffset ? NDArm64eSignedIBWithCFAPC
                                                   : NDArm64eSignedIBWithCFA);
      if (SigningOffset)
        EXPECT_EQ(Plan->Frame.SigningPC, VA + *SigningOffset);
      EXPECT_EQ(Plan->Frame.CFAFromFP, !Compact);
      EXPECT_EQ(Plan->Frame.SavedFP, !Frameless);
      EXPECT_EQ(Plan->Frame.SavedLR, !Frameless);
      EXPECT_EQ(Plan->Frame.CFAOffset,
                Compact
                    ? kCompactFrameDepth + (ExtraStack ? kCompactExtraDepth : 0)
                    : kCFAOffset);
      EXPECT_EQ(Plan->Frame.FPOffset, Frameless ? 0 : kFPOffset);
      EXPECT_EQ(Plan->Frame.LROffset, Frameless ? 0 : kLROffset);
      const auto GPRCount =
          Compact
              ? llvm::count_if(CompactSpills,
                               [](const auto &Spill) {
                                 return Spill.first < aarch64_unwind::kDwarfV0;
                               })
              : NDArm64ePreservedGPRCount;
      ASSERT_EQ(Plan->Count, GPRCount);
      Rules << int64_t(VA) - int64_t(Public->Addr) << ' ' << *Offset << ' '
            << *Offset + 2 * aarch64::kInsnSize << ' ' << Plan->Frame.CFAFromFP
            << ' ' << Plan->Frame.SavedFP << ' ' << Plan->Frame.SavedLR << ' '
            << Plan->Frame.CFAOffset << ' ' << Plan->Frame.FPOffset << ' '
            << Plan->Frame.LROffset << ' ' << Plan->Count << ' '
            << Plan->Vectors.LowMask << ' ' << Plan->Vectors.HighMask << ' '
            << unsigned(Plan->Frame.Signing) << ' '
            << (SigningOffset ? Plan->Frame.SigningPC - VA : 0) << '\n';
      for (unsigned I = 0; I < Plan->Count; ++I) {
        EXPECT_EQ(Plan->Saved[I].Register, Compact
                                               ? CompactSpills[I].first
                                               : NDArm64eFirstPreservedGPR + I);
        EXPECT_EQ(Plan->Saved[I].CFAOffset,
                  Compact ? CompactSpills[I].second
                          : kFirstGPROffset -
                                int(I / 2 * 2) * int(sizeof(uint64_t)) +
                                int(I % 2) * int(sizeof(uint64_t)));
        Rules << Plan->Saved[I].Register << ' ' << Plan->Saved[I].CFAOffset
              << '\n';
      }
      for (unsigned I = 0; I < NDArm64ePreservedDCount; ++I) {
        const int Width = (Quad ? 2 : 1) * sizeof(uint64_t);
        int Expected = (Quad ? kFirstQOffset : kFirstDOffset) -
                       int(I / 2 * 2) * Width + int(I % 2) * Width;
        if (Compact) {
          const auto Slot =
              llvm::find_if(CompactSpills, [&](const auto &Spill) {
                return Spill.first ==
                       aarch64_unwind::kDwarfV0 + NDArm64eFirstPreservedD + I;
              });
          Expected = Slot == CompactSpills.end() ? 0 : Slot->second;
        }
        EXPECT_EQ(Plan->Vectors.LowOffsets[I], Expected);
        if (Quad)
          EXPECT_EQ(Plan->Vectors.HighOffsets[I],
                    Expected + int(sizeof(uint64_t)));
        Rules << Plan->Vectors.LowOffsets[I] << ' '
              << Plan->Vectors.HighOffsets[I] << '\n';
      }
    }
    Rules.close();
    ASSERT_TRUE(Rules);
    if (Execute) {
      ASSERT_EQ(llvm::sys::ExecuteAndWait(
                    Driver, {Driver, ExecutionPath, RulesPath,
                             std::to_string(Signal && Compact &&
                                            (InstalledImage || !Frameless))}),
                0);
      ++Executions;
    }
  }
  auto Unchanged = Reader->load(Input);
  ASSERT_TRUE(bool(Unchanged)) << llvm::toString(Unchanged.takeError());
  EXPECT_EQ(Unchanged->Raw, Snapshot);
  if (SigningOffset)
    testing::Test::RecordProperty(
        kUnsignedFixtureHash, llvm::toHex(llvm::SHA256::hash(Snapshot), true));
  testing::Test::RecordProperty(kNativeImages, Executions);
  if (Frameless && Signal)
    testing::Test::RecordProperty(kSystemSteps,
                                  Executions / 2 * Functions * kSignalRounds);
  if (SigningOffset)
    testing::Test::RecordProperty(kPAuthLRImages, 2);
  if (Compact && !Frameless && Signal)
    testing::Test::RecordProperty(SigningOffset ? kPAuthLRSystemSteps
                                                : kSavedFrameSystemSteps,
                                  Executions * Functions * kSignalRounds);
  testing::Test::RecordProperty(Signal ? kResumptions : kObservedCalls,
                                Executions * Functions *
                                    (Signal ? kSignalRounds : kNormalRounds));
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64SavedRegisters, PublishesAllPreservedGPRAndDRegisters) {
  verifySavedRegisters(false, false);
}
TEST(AArch64SavedRegisters, PublishesAllPreservedGPRAndQRegisters) {
  verifySavedRegisters(true, false);
}
TEST(AArch64SavedRegisters, SignalResumesPublishedGPRAndDRegisters) {
  verifySavedRegisters(false, true);
}
TEST(AArch64SavedRegisters, SignalResumesPublishedGPRAndQRegisters) {
  verifySavedRegisters(true, true);
}
TEST(AArch64CompactRegisters, PublishesAllPreservedPairs) {
  verifySavedRegisters(false, false, kCompactAllPairs);
}
TEST(AArch64CompactRegisters, PublishesSparsePreservedPairs) {
  verifySavedRegisters(false, false, kCompactSparsePairs);
}
TEST(AArch64CompactRegisters, PublishesAllPairsWithAdditionalStack) {
  verifySavedRegisters(false, false, kCompactAllPairs, true);
}
TEST(AArch64CompactRegisters, PublishesSparsePairsWithAdditionalStack) {
  verifySavedRegisters(false, false, kCompactSparsePairs, true);
}
TEST(AArch64CompactRegisters, SignalResumesAllPreservedPairs) {
  verifySavedRegisters(false, true, kCompactAllPairs);
}
TEST(AArch64CompactRegisters, SignalResumesSparsePreservedPairs) {
  verifySavedRegisters(false, true, kCompactSparsePairs);
}
TEST(AArch64CompactRegisters, SignalResumesAllPairsWithAdditionalStack) {
  verifySavedRegisters(false, true, kCompactAllPairs, true);
}
TEST(AArch64CompactRegisters, SignalResumesSparsePairsWithAdditionalStack) {
  verifySavedRegisters(false, true, kCompactSparsePairs, true);
}
TEST(AArch64CompactRegisters, PublishesEachPairAndFrameOnly) {
  verifySavedRegisters(false, false, 0);
  for (const auto &Pair : kCompactPairCases) {
    SCOPED_TRACE(Pair.Mask);
    verifySavedRegisters(false, false, Pair.Mask);
  }
  const auto Executions = std::getenv(kExecute) ? 2 : 0;
  testing::Test::RecordProperty(kNativeImages,
                                Executions * (1 + kCompactPairCases.size()));
  testing::Test::RecordProperty(kObservedCalls,
                                Executions * (1 + kCompactPairCases.size()) *
                                    kNormalRounds);
}
TEST(AArch64CompactRegisters, PublishesCoalescedAllPairFrames) {
  verifySavedRegisters(false, false, kCompactAllPairs, true, true);
}
TEST(AArch64CompactRegisters, PublishesCoalescedSparseFrames) {
  verifySavedRegisters(false, false, kCompactSparsePairs, false, true);
}
TEST(AArch64CompactRegisters, SignalResumesCoalescedAllPairFrames) {
  verifySavedRegisters(false, true, kCompactAllPairs, true, true);
}
TEST(AArch64CompactRegisters, SignalResumesCoalescedSparseFrames) {
  verifySavedRegisters(false, true, kCompactSparsePairs, false, true);
}
TEST(AArch64CompactRegisters, PublishesFramelessAllPairs) {
  verifySavedRegisters(false, false, kCompactAllPairs, false, false, true);
}
TEST(AArch64CompactRegisters, PublishesFramelessSparsePairs) {
  verifySavedRegisters(false, false, kCompactSparsePairs, true, false, true);
}
TEST(AArch64CompactRegisters, SignalResumesFramelessAllPairs) {
  verifySavedRegisters(false, true, kCompactAllPairs, false, false, true);
}
TEST(AArch64CompactRegisters, SignalResumesFramelessSparsePairs) {
  verifySavedRegisters(false, true, kCompactSparsePairs, true, false, true);
}
TEST(AArch64CompactRegisters, PublishesCoalescedFramelessPairs) {
  verifySavedRegisters(false, false, kCompactAllPairs, true, true, true);
}
TEST(AArch64CompactRegisters, SignalResumesCoalescedFramelessPairs) {
  verifySavedRegisters(false, true, kCompactSparsePairs, true, true, true);
}
TEST(AArch64CompactRegisters, PublishesEachFramelessPair) {
  for (const auto &Pair : kCompactPairCases) {
    SCOPED_TRACE(Pair.Mask);
    verifySavedRegisters(false, false, Pair.Mask, false, false, true);
  }
  const auto Executions = std::getenv(kExecute) ? 2 : 0;
  testing::Test::RecordProperty(kNativeImages,
                                Executions * kCompactPairCases.size());
  testing::Test::RecordProperty(
      kObservedCalls, Executions * kCompactPairCases.size() * kNormalRounds);
}

TEST(AArch64CompactRegisters, SystemUnwindsEachFramelessPair) {
  for (const auto &Pair : kCompactPairCases) {
    SCOPED_TRACE(Pair.Mask);
    verifySavedRegisters(false, true, Pair.Mask, false, false, true);
  }
  const auto Executions = std::getenv(kExecute) ? 2 : 0;
  testing::Test::RecordProperty(kNativeImages,
                                Executions * kCompactPairCases.size());
  testing::Test::RecordProperty(
      kResumptions, Executions * kCompactPairCases.size() * kSignalRounds);
  testing::Test::RecordProperty(
      kSystemSteps, Executions / 2 * kCompactPairCases.size() * kSignalRounds);
}

TEST(AArch64CompactRegisters, PublishesFramelessLanguageFrame) {
  verifySavedRegisters(false, false, 0, false, false, true, std::nullopt, true);
}
TEST(AArch64CompactRegisters, PublishesFramelessLanguageSavedPairs) {
  verifySavedRegisters(false, false, kCompactAllPairs, false, false, true,
                       std::nullopt, true);
}
TEST(AArch64CompactRegisters, SystemUnwindsFramelessLanguageSavedPairs) {
  verifySavedRegisters(false, true, kCompactSparsePairs, true, false, true,
                       std::nullopt, true);
}

TEST(AArch64CompactRegisters, PublishesPAuthLRAllPairs) {
  verifySavedRegisters(false, false, kCompactAllPairs, false, false, false, 0);
}

TEST(AArch64CompactRegisters, PublishesPAuthLROffsetAndSparsePairs) {
  verifySavedRegisters(false, false, kCompactSparsePairs, true, false, false,
                       kPAuthLRNonzeroOffset);
}
TEST(AArch64CompactRegisters, PublishesPAuthLRFrameOnlyWithWidePC) {
  verifySavedRegisters(false, false, 0, false, false, false,
                       kPAuthLRWideOffset);
}

TEST(AArch64CompactRegisters, PublishesPAuthLRLanguageCleanup) {
  verifySavedRegisters(false, false, kCompactSparsePairs, true, false, false,
                       kPAuthLRNonzeroOffset, true);
}

TEST(AArch64CompactRegisters, PublishesPAuthLRBesideIndexedCleanup) {
  verifySavedRegisters(false, false, kCompactSparsePairs, true, false, false,
                       kPAuthLRNonzeroOffset, false, true);
}

void verifyMixedCleanupOwners(bool Shared) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kDirectory, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Assembly = (Root / kSource).string();
  const auto Original = (Root / kOriginal).string();
  std::ofstream(Assembly) << kCompactBody << kIndexedPeerBody;
  const auto Pairs =
      std::string(kCompactFlag) + std::to_string(kCompactSparsePairs);
  const auto Signing =
      std::string(kPAuthLRFlag) + std::to_string(kPAuthLRNonzeroOffset);
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(
          kCompiler, {kCompiler, kTargetFlag, kTarget, kOptimize, kDylib,
                      kTight, Pairs, Signing, kCoalescedFlag, kLanguageFlag,
                      Shared ? kSharedCleanupFlag : kDistinctCleanupFlag,
                      Assembly, kCXXRuntime, kOutputFlag, Original}),
      0);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                kFixtureCodesign,
                {kFixtureCodesign, kFixtureRemoveSignature, Original}),
            0);
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Input = Reader->load(Original);
  ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
  const auto *Sum = Input->findSymbol(kSumSymbol);
  ASSERT_NE(Sum, nullptr);
  const auto *Unwind = Input->ExceptionMetadata.findFunctionEntry(Sum->Addr);
  ASSERT_TRUE(Unwind && Unwind->Compact);
  const uint32_t Encoding =
      kCompactPAuthLRMode | kCompactSparsePairs |
      (kPAuthLRNonzeroOffset / aarch64::kInsnSize << kPAuthLRShift) |
      (Unwind->Compact->NativeEncoding &
       (macho_unwind::kPersonalityMask | macho_unwind::kHasLSDA));
  ASSERT_NO_FATAL_FAILURE(
      writeCompactFixture(Original, *Input, Encoding, true));

  const std::array Symbols = {kSumSymbol, kDifferenceSymbol,
                              kFirstIndexedSymbol, kSecondIndexedSymbol};
  using Addresses = std::array<va_t, Symbols.size()>;
  Addresses Entries{};
  for (auto [I, Name] : llvm::enumerate(Symbols)) {
    const auto *Sym = Input->findSymbol(Name);
    ASSERT_NE(Sym, nullptr);
    Entries[I] = Sym->Addr;
  }
  struct CleanupSite {
    va_t Begin, End, Pad;
    bool operator==(const CleanupSite &) const = default;
  };
  std::array<CleanupSite, Symbols.size()> Sites{};
  const auto Check = [&](const BinaryImage &Image, const Addresses &Owners,
                         bool First) {
    std::optional<va_t> SharedCIE, FirstLSDA;
    for (auto [I, Start] : llvm::enumerate(Owners)) {
      SCOPED_TRACE(Symbols[I]);
      const auto *F = Image.ExceptionMetadata.findFunctionEntry(Start);
      ASSERT_TRUE(F && F->Compact && F->Itanium);
      ASSERT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);
      ASSERT_EQ(F->Personality, ExceptionPersonality::GxxPersonalityV0);
      ASSERT_EQ(F->Itanium->CallSites.size(), 1u);
      ASSERT_TRUE(F->Itanium->isCleanupOnly());
      const auto &Site = F->Itanium->CallSites.front();
      const CleanupSite Relative{Site.GuardedRange.Begin - Start,
                                 Site.GuardedRange.End - Start,
                                 Site.LandingPadVA - Start};
      if (First)
        Sites[I] = Relative;
      else
        EXPECT_EQ(Relative, Sites[I]);
      if (I < kFunctions) {
        EXPECT_FALSE(F->Dwarf);
        EXPECT_EQ(F->Compact->NativeEncoding, Encoding);
        ASSERT_EQ(F->Compact->ReturnAddressSigningOffset,
                  kPAuthLRNonzeroOffset);
        const auto *Sign =
            Image.readVA(Start + kPAuthLRNonzeroOffset, aarch64::kInsnSize);
        ASSERT_NE(Sign, nullptr);
        EXPECT_EQ(readLE<uint32_t>(Sign), kSigningInstruction);
        continue;
      }
      ASSERT_TRUE(F->Dwarf);
      EXPECT_EQ(F->Compact->Kind, CompactUnwindKind::DwarfFDE);
      EXPECT_EQ(F->Compact->DwarfFDEOffset, F->Dwarf->SectionOffset);
      EXPECT_EQ(F->Dwarf->InitialLocation, Start);
      EXPECT_EQ(F->Dwarf->LSDAVA, F->Itanium->LSDAVA);
      ASSERT_TRUE(Image.ExceptionMetadata.findCIE(F->Dwarf->CIESectionOffset));
      if (SharedCIE) {
        EXPECT_EQ(F->Dwarf->CIESectionOffset, *SharedCIE);
        EXPECT_EQ(F->Dwarf->LSDAVA == *FirstLSDA, Shared);
      } else {
        SharedCIE = F->Dwarf->CIESectionOffset;
        FirstLSDA = F->Dwarf->LSDAVA;
      }
    }
  };

  unsigned Publications = 0;
  for (bool NoOpt : {true, false}) {
    SCOPED_TRACE(NoOpt);
    auto Source = Original;
    auto Owners = Entries;
    for (const char *Name : {kPublished, kRepublished}) {
      SCOPED_TRACE(Name);
      auto Before = Reader->load(Source);
      ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
      if (Source == Original)
        ASSERT_NO_FATAL_FAILURE(Check(*Before, Owners, true));
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.PatchMode = true;
      Options.EmitDumpOutput = false;
      Options.NoOpt = NoOpt;
      auto Lifted = Pipeline().run(*Before, Context, Options);
      ASSERT_TRUE(Lifted.Success && Lifted.LlvmModule) << Lifted.Error;
      symbolizeImageAbsolutePointers(*Lifted.LlvmModule, *Before);
      MachOPatcher Patcher;
      Patcher.setImageContext(&*Before);
      const auto Output = (Root / Name).string();
      const auto Installed =
          Patcher.patch(Source, Output, *Lifted.LlvmModule, Before->Arch);
      ASSERT_TRUE(Installed.Success);
      for (auto &Owner : Owners) {
        const auto It =
            llvm::find_if(Installed.PatchedFunctions,
                          [&](const auto &F) { return F.OriginalVA == Owner; });
        ASSERT_NE(It, Installed.PatchedFunctions.end());
        ASSERT_NE(It->OwnerVA, Owner);
        Owner = It->OwnerVA;
      }
      auto After = Reader->load(Output);
      ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
      ASSERT_NO_FATAL_FAILURE(Check(*After, Owners, false));
      ++Publications;
      Source = Output;
    }
    ASSERT_TRUE(std::filesystem::remove(Root / kPublished));
    ASSERT_TRUE(std::filesystem::remove(Root / kRepublished));
  }
  testing::Test::RecordProperty(kMixedPublications, Publications);
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64CompactRegisters, RepublishesPAuthLRBesideSharedCleanupTable) {
  verifyMixedCleanupOwners(true);
}

TEST(AArch64CompactRegisters, RepublishesPAuthLRBesideDistinctCleanupTables) {
  verifyMixedCleanupOwners(false);
}

TEST(AArch64CompactRegisters, SystemUnwindsPAuthLRAllPairs) {
  verifySavedRegisters(false, true, kCompactAllPairs, false, false, false, 0);
}

TEST(AArch64CompactRegisters, SystemUnwindsPAuthLROffsetAndSparsePairs) {
  verifySavedRegisters(false, true, kCompactSparsePairs, true, false, false,
                       kPAuthLRNonzeroOffset);
}
} // namespace
