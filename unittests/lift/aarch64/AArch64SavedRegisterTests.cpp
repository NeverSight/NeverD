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
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
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

void verifySavedRegisters(
    bool Quad, bool Signal, std::optional<unsigned> CompactPairs = std::nullopt,
    bool ExtraStack = false, bool Coalesced = false, bool Frameless = false,
    std::optional<unsigned> SigningOffset = std::nullopt) {
#if defined(__APPLE__) && defined(__aarch64__)
#if !__has_include("llvm/BinaryFormat/AArch64CompactUnwind.def")
  if (SigningOffset)
    GTEST_SKIP() << kPAuthLRMCUnavailable;
#endif
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
  std::ofstream(Source) << (Compact ? kCompactBody : kBody);
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
  auto Compile = Common;
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

  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Image = Reader->load(Input);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
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
    ASSERT_NE(At, std::string::npos);
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
  ASSERT_EQ(Installed.PatchedFunctions.size(), Functions);
  auto Published = Reader->load(Output);
  ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());
  EXPECT_TRUE(Published->isArm64e());
  EXPECT_NE(Published->Raw, Snapshot);
  for (const auto &Entry : Installed.PatchedFunctions) {
    EXPECT_NE(Entry.OwnerVA, Entry.OriginalVA);
    const auto *Before =
        Image->ExceptionMetadata.findFunction(Entry.OriginalVA);
    const auto *After =
        Published->ExceptionMetadata.findFunction(Entry.OwnerVA);
    ASSERT_TRUE(Before && After);
    if (Compact) {
      ASSERT_TRUE(Before->Compact && After->Compact);
      EXPECT_FALSE(Before->Dwarf);
      EXPECT_FALSE(After->Dwarf);
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
      EXPECT_EQ(Before->Compact->NativeEncoding, Mode | *CompactPairs);
      EXPECT_EQ(After->Compact->NativeEncoding,
                Before->Compact->NativeEncoding);
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
    const auto &View = InstalledImage ? *Published : *Image;
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
            << Plan->Vectors.LowMask << ' ' << Plan->Vectors.HighMask << '\n';
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
    if (!SigningOffset && std::getenv(kExecute)) {
      ASSERT_EQ(
          llvm::sys::ExecuteAndWait(
              Driver, {Driver, InstalledImage ? Output : Input, RulesPath}),
          0);
      ++Executions;
    }
  }
  auto Unchanged = Reader->load(Input);
  ASSERT_TRUE(bool(Unchanged)) << llvm::toString(Unchanged.takeError());
  EXPECT_EQ(Unchanged->Raw, Snapshot);
  testing::Test::RecordProperty(
      kOriginalHash, llvm::toHex(llvm::SHA256::hash(Snapshot), true));
  testing::Test::RecordProperty(
      kPublishedHash, llvm::toHex(llvm::SHA256::hash(Published->Raw), true));
  testing::Test::RecordProperty(kNativeImages, Executions);
  if (SigningOffset)
    testing::Test::RecordProperty(kPAuthLRImages, 2);
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

TEST(AArch64CompactRegisters, PublishesPAuthLRAllPairs) {
  verifySavedRegisters(false, false, kCompactAllPairs, false, false, false, 0);
}

TEST(AArch64CompactRegisters, PublishesPAuthLROffsetAndSparsePairs) {
  verifySavedRegisters(false, false, kCompactSparsePairs, true, false, false,
                       kPAuthLRNonzeroOffset);
}
} // namespace
