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

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace {
using namespace neverd;
#define SAVED_TEXT(Name, Text) constexpr char Name[] = Text;
#define SAVED_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64SavedRegisterFixtures.def"

void verifySavedRegisters(bool Quad, bool Signal) {
#if defined(__APPLE__) && defined(__aarch64__)
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
  std::ofstream(Source) << kBody;
  std::ofstream(Wrapper) << kObserver;
  std::ofstream(Caller) << kDriverBody;
  llvm::SmallVector<llvm::StringRef> Common{kCompiler, kTargetFlag, kTarget,
                                            kOptimize};
  if (Quad)
    Common.push_back(kQuadFlag);
  if (Signal)
    Common.push_back(kSignalFlag);
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
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.PatchMode = true;
  Options.EmitDumpOutput = false;
  auto Lifted = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Lifted.Success) << Lifted.Error;
  ASSERT_TRUE(Lifted.LlvmModule);
  auto &Module = *Lifted.LlvmModule;
  symbolizeImageAbsolutePointers(Module, *Image);
  for (const auto *Name : {kSum, kDifference}) {
    const auto *Function = Module.getFunction(Name);
    ASSERT_NE(Function, nullptr);
    ASSERT_TRUE(Function->hasFnAttribute(llvm::Attribute::Naked));
  }

  MachOPatcher Patcher;
  Patcher.setImageContext(&*Image);
  // The matching instructions alone do not authorize changed unwind locations.
  for (const auto &[Before, After] :
       {std::pair{kCFIX19, kWrongCFIX19},
        std::pair{Quad ? kCFIQ8 : kCFID8, Quad ? kWrongCFIQ8 : kWrongCFID8}}) {
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
  ASSERT_EQ(Installed.PatchedFunctions.size(), kFunctions);
  auto Published = Reader->load(Output);
  ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());
  EXPECT_TRUE(Published->isArm64e());
  for (const auto &Entry : Installed.PatchedFunctions) {
    const auto *Before =
        Image->ExceptionMetadata.findFunction(Entry.OriginalVA);
    const auto *After =
        Published->ExceptionMetadata.findFunction(Entry.OwnerVA);
    ASSERT_TRUE(Before && Before->Dwarf && After && After->Dwarf);
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
      for (uint64_t At = 0; At < Frame->CodeRange.size();
           At += aarch64::kInsnSize) {
        const auto *Code = View.readVA(VA + At, aarch64::kInsnSize);
        ASSERT_NE(Code, nullptr);
        if (readLE<uint32_t>(Code) == Wanted) {
          ASSERT_FALSE(Offset);
          Offset = At;
        }
      }
      ASSERT_TRUE(Offset);
      const auto &Row = aarch64UnwindRowAt(*Rows, *Offset);
      const auto Plan =
          prepareDarwinArm64eSystemCaller(Row, Frame->CodeRange, VA);
      ASSERT_TRUE(Plan);
      ASSERT_TRUE(Plan->Vectors.Complete);
      EXPECT_EQ(Plan->Vectors.LowMask, kVectorMask);
      EXPECT_EQ(Plan->Vectors.HighMask, Quad ? kVectorMask : 0u);
      EXPECT_EQ(Plan->Frame.Signing, NDArm64eSignedIBWithCFA);
      EXPECT_TRUE(Plan->Frame.CFAFromFP && Plan->Frame.SavedFP &&
                  Plan->Frame.SavedLR);
      EXPECT_EQ(Plan->Frame.CFAOffset, kCFAOffset);
      EXPECT_EQ(Plan->Frame.FPOffset, kFPOffset);
      EXPECT_EQ(Plan->Frame.LROffset, kLROffset);
      ASSERT_EQ(Plan->Count, NDArm64ePreservedGPRCount);
      Rules << int64_t(VA) - int64_t(Public->Addr) << ' ' << *Offset << ' '
            << *Offset + 2 * aarch64::kInsnSize << ' ' << Plan->Frame.CFAOffset
            << ' ' << Plan->Frame.FPOffset << ' ' << Plan->Frame.LROffset << ' '
            << Plan->Vectors.LowMask << ' ' << Plan->Vectors.HighMask << '\n';
      for (unsigned I = 0; I < Plan->Count; ++I) {
        EXPECT_EQ(Plan->Saved[I].Register, NDArm64eFirstPreservedGPR + I);
        EXPECT_EQ(Plan->Saved[I].CFAOffset,
                  kFirstGPROffset - int(I / 2 * 2) * int(sizeof(uint64_t)) +
                      int(I % 2) * int(sizeof(uint64_t)));
        Rules << Plan->Saved[I].Register << ' ' << Plan->Saved[I].CFAOffset
              << '\n';
      }
      for (unsigned I = 0; I < NDArm64ePreservedDCount; ++I) {
        const int Width = (Quad ? 2 : 1) * sizeof(uint64_t);
        const int Expected = (Quad ? kFirstQOffset : kFirstDOffset) -
                             int(I / 2 * 2) * Width + int(I % 2) * Width;
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
    if (std::getenv(kExecute)) {
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
  testing::Test::RecordProperty(Signal ? kResumptions : kObservedCalls,
                                Executions * kFunctions *
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
} // namespace
