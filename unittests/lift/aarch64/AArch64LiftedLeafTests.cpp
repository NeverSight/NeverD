//===- AArch64LiftedLeafTests.cpp - Native lifted leaf publication --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/llvm/AArch64/LLVMAArch64ReturnProtection.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/loader/Loader.h"
#include "neverd/object/SectionNames.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using namespace neverd;
#define A64_LEAF_TEXT(Name, Text) constexpr char Name[] = Text;
#define A64_LEAF_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64LiftedLeafFixtures.def"

void verifyPublishedLeaves(const char *Leaves, const char *CallerSource,
                           const char *SourceName = kSource) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kTemporary, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / SourceName).string();
  const auto Input = (Root / kOriginal).string();
  const auto Output = (Root / kOutput).string();
  const auto Rejected = (Root / kRejected).string();
  std::ofstream(Source) << Leaves;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                kCompiler, {kCompiler, kTargetFlag, kTarget, kOptimize,
                            kOmitFrame, kDylib, kTight, Source, kOut, Input}),
            0);
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
  ASSERT_TRUE(Module.getModuleFlag(kArm64eAnalysisModuleFlag));

  MachOPatcher Patcher;
  Patcher.setImageContext(&*Image);
  auto Partial = llvm::CloneModule(Module);
  ASSERT_NE(Partial->getFunction(kDifference), nullptr);
  Partial->getFunction(kDifference)->deleteBody();
  EXPECT_FALSE(Patcher.patch(Input, Rejected, *Partial, Image->Arch).Success);
  EXPECT_FALSE(std::filesystem::exists(Rejected));

  auto Changed = llvm::CloneModule(Module);
  auto *Sum = Changed->getFunction(kSum);
  ASSERT_NE(Sum, nullptr);
  auto *Result = Sum->getReturnType();
  ASSERT_TRUE(Result->isIntegerTy() || Result->isFloatingPointTy());
  auto *Wrong = Result->isIntegerTy()
                    ? llvm::ConstantInt::get(Result, kTamperedResult)
                    : llvm::ConstantFP::get(Result, kTamperedResult);
  for (auto &Block : *Sum)
    if (auto *R = llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator()))
      R->setOperand(0, Wrong);
  EXPECT_FALSE(Patcher.patch(Input, Rejected, *Changed, Image->Arch).Success);
  EXPECT_FALSE(std::filesystem::exists(Rejected));

  if (Module.getFunction(kSum)->hasFnAttribute(kReturnSigningScopeAttribute)) {
    auto Unsigned = llvm::CloneModule(Module);
    auto *Entry = Unsigned->getFunction(kSum);
    Entry->removeFnAttr(kReturnSigningScopeAttribute);
    Entry->removeFnAttr(kReturnSigningKeyAttribute);
    EXPECT_FALSE(
        Patcher.patch(Input, Rejected, *Unsigned, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }

  auto Installed = Patcher.patch(Input, Output, Module, Image->Arch);
  ASSERT_TRUE(Installed.Success);
  ASSERT_EQ(Installed.PatchedFunctions.size(), kCount);
  auto Final = Reader->load(Output);
  ASSERT_TRUE(bool(Final)) << llvm::toString(Final.takeError());
  EXPECT_TRUE(Final->isArm64e());
  EXPECT_NE(Final->Raw, Snapshot);
  for (const auto &Entry : Installed.PatchedFunctions) {
    EXPECT_NE(Entry.OwnerVA, Entry.OriginalVA);
    EXPECT_TRUE(Final->ExceptionMetadata.findFunction(Entry.OwnerVA));
  }
  auto Original = Reader->load(Input);
  ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
  EXPECT_EQ(Original->Raw, Snapshot);
  EXPECT_TRUE(Module.getModuleFlag(kArm64eAnalysisModuleFlag));
  EXPECT_FALSE(Codegen().compile(Module, Image->Arch, Image->Format).Success);
  testing::Test::RecordProperty(
      kOriginalHash, llvm::toHex(llvm::SHA256::hash(Snapshot), true));
  testing::Test::RecordProperty(
      kPublishedHash, llvm::toHex(llvm::SHA256::hash(Final->Raw), true));
  const auto Caller = (Root / kDriverSource).string();
  const auto Driver = (Root / kDriver).string();
  std::ofstream(Caller) << CallerSource;
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(kCompiler, {kCompiler, kTargetFlag, kTarget,
                                            kOptimize, Caller, kOut, Driver}),
      0);
  auto CallerImage = Reader->load(Driver);
  ASSERT_TRUE(bool(CallerImage)) << llvm::toString(CallerImage.takeError());
  ASSERT_TRUE(CallerImage->isArm64e());
  const auto *Text = CallerImage->findSectionByName(section_names::macho::Text);
  ASSERT_NE(Text, nullptr);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*CallerImage));
  unsigned AuthenticatedCalls = 0;
  for (uint64_t Off = 0; Off + aarch64::kInsnSize <= Text->FileSz;
       Off += aarch64::kInsnSize) {
    DecodedInsn Insn{};
    ASSERT_EQ(Decode.decodeOne(CallerImage->Raw.data() + Text->FileOff + Off,
                               aarch64::kInsnSize, Text->VA + Off, Insn),
              aarch64::kInsnSize);
    if (Insn.Id == AARCH64_INS_BLRAA || Insn.Id == AARCH64_INS_BLRAAZ ||
        Insn.Id == AARCH64_INS_BLRAB || Insn.Id == AARCH64_INS_BLRABZ)
      ++AuthenticatedCalls;
  }
  ASSERT_GE(AuthenticatedCalls, kCount);
  testing::Test::RecordProperty(kAuthenticatedSites, AuthenticatedCalls);
  if (std::getenv(kExecute)) {
    ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, Input}), 0);
    ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, Output}), 0);
    testing::Test::RecordProperty(kNativeImages, kCount);
  }
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64LiftedImage, VerifiedLeafBodiesUseTheNormalPatchPipeline) {
  verifyPublishedLeaves(kLeaves, kCaller);
}

TEST(AArch64DarwinEntry, SwiftContextSurvivesNativeLeafPublication) {
  verifyPublishedLeaves(kSwiftLeaves, kSwiftCaller);
}

TEST(AArch64DarwinEntry, ScalarFPParametersAndResultsSurvivePublication) {
  verifyPublishedLeaves(kScalarFPLeaves, kScalarFPCaller);
}

TEST(AArch64DarwinEntry, SignedLeafUsesTheIncomingSPAtItsNewAddress) {
  verifyPublishedLeaves(kSignedLeaves, kCaller, kAssemblyName);
}

TEST(AArch64LiftedMemory, BaseAndDisplacementRetainLoadAndStoreEffects) {
  verifyPublishedLeaves(kMemoryLeaves, kMemoryCaller);
}
} // namespace
