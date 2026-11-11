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
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {
using namespace neverd;
#define A64_LEAF_TEXT(Name, Text) constexpr char Name[] = Text;
#define A64_LEAF_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64LiftedLeafFixtures.def"

#if defined(__APPLE__) && defined(__aarch64__)
void verifyTailTargets(llvm::Module &Module, MachOPatcher &Patcher,
                       const std::string &Input, const std::string &Rejected,
                       const std::string &ReorderedPath, Arch Architecture) {
  auto Candidate = llvm::CloneModule(Module);
  auto *Forwarder = Candidate->getFunction(kDifference);
  auto *Callee = Candidate->getFunction(kSum);
  ASSERT_NE(Forwarder, nullptr);
  ASSERT_NE(Callee, nullptr);
  auto *Return =
      llvm::dyn_cast<llvm::ReturnInst>(Forwarder->back().getTerminator());
  ASSERT_NE(Return, nullptr);
  auto *Call = llvm::dyn_cast_or_null<llvm::CallInst>(Return->getReturnValue());
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->getCalledFunction(), Callee);
  ASSERT_TRUE(Call->isTailCall());
  Call->setCalledFunction(Forwarder);
  EXPECT_FALSE(
      Patcher.patch(Input, Rejected, *Candidate, Architecture).Success);
  EXPECT_FALSE(std::filesystem::exists(Rejected));

  Call->setCalledFunction(Callee);
  auto &Functions = Candidate->getFunctionList();
  Functions.splice(Functions.begin(), Functions, Forwarder->getIterator());
  auto Reordered =
      Patcher.patch(Input, ReorderedPath, *Candidate, Architecture);
  ASSERT_TRUE(Reordered.Success);
  ASSERT_EQ(Reordered.PatchedFunctions.size(), kCount);
  llvm::sort(Reordered.PatchedFunctions, [](const auto &A, const auto &B) {
    return A.OriginalVA < B.OriginalVA;
  });
  const auto &Body = Reordered.PatchedFunctions.front();
  const auto &Tail = Reordered.PatchedFunctions.back();
  ASSERT_GT(Tail.OriginalVA, Body.OriginalVA);
  ASSERT_LT(Tail.OwnerVA, Body.OwnerVA);
  auto Final = Loader::create(BinaryFormat::MachO)->load(ReorderedPath);
  ASSERT_TRUE(bool(Final)) << llvm::toString(Final.takeError());
  auto *Word = Final->readVA(Tail.OwnerVA, aarch64::kInsnSize);
  ASSERT_NE(Word, nullptr);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Final));
  DecodedInsn Insn{};
  ASSERT_EQ(Decode.decodeOne(Word, aarch64::kInsnSize, Tail.OwnerVA, Insn),
            aarch64::kInsnSize);
  ASSERT_EQ(Insn.Id, AARCH64_INS_B);
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  const auto &Branch = Insn.Raw->detail->aarch64;
  ASSERT_EQ(Branch.op_count, 1);
  ASSERT_EQ(Branch.operands[0].type, AARCH64_OP_IMM);
  EXPECT_EQ(static_cast<va_t>(Branch.operands[0].imm), Body.OwnerVA);
}
#endif

void verifyPublishedLeaves(const char *Leaves, const char *CallerSource,
                           const char *SourceName = kSource,
                           std::string_view Preamble = {},
                           bool TailForwarder = false) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kTemporary, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / SourceName).string();
  const auto Input = (Root / kOriginal).string();
  const auto Output = (Root / kOutput).string();
  const auto Rejected = (Root / kRejected).string();
  std::ofstream(Source) << Preamble << Leaves;
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
  symbolizeImageAbsolutePointers(Module, *Image);
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

  const auto ReorderedPath = (Root / kReorderedOutput).string();
  if (TailForwarder)
    verifyTailTargets(Module, Patcher, Input, Rejected, ReorderedPath,
                      Image->Arch);
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
  std::ofstream(Caller) << Preamble << CallerSource;
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
    if (TailForwarder) {
      ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, ReorderedPath}), 0);
      testing::Test::RecordProperty(kReorderedTail, true);
    }
    testing::Test::RecordProperty(kNativeImages, kCount);
  }
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

// Exercise a copy of the host's own executable. The original platform
// signature belongs to Apple; the unsigned input and the separately signed
// test results live only in this test directory.
void verifyPublishedDarwinExecutable(const char *SystemPath, int ExitCode) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kTemporary, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Slice = (Root / kDarwinSlice).string();
  const auto Input = (Root / kUnsignedInput).string();
  const auto Output = (Root / kDarwinOutput).string();
  auto System = llvm::MemoryBuffer::getFile(SystemPath);
  ASSERT_TRUE(bool(System));
  if (llvm::identify_magic((*System)->getBuffer()) ==
      llvm::file_magic::macho_universal_binary)
    ASSERT_EQ(
        llvm::sys::ExecuteAndWait(kLipo, {kLipo, SystemPath, kThinOption,
                                          kDarwinArch, kLipoOutput, Slice}),
        0);
  else
    ASSERT_TRUE(std::filesystem::copy_file(SystemPath, Slice));
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Platform = Reader->load(Slice);
  ASSERT_TRUE(bool(Platform)) << llvm::toString(Platform.takeError());
  ASSERT_TRUE(Platform->isArm64e());
  ASSERT_TRUE(std::filesystem::copy_file(Slice, Input));
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCodesign,
                                      {kCodesign, kRemoveSignature, Input}),
            0);
  auto Image = Reader->load(Input);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.PatchMode = true;
  Options.EmitDumpOutput = false;
  auto Lifted = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Lifted.Success) << Lifted.Error;
  ASSERT_TRUE(Lifted.LlvmModule);
  symbolizeImageAbsolutePointers(*Lifted.LlvmModule, *Image);
  MachOPatcher Patcher;
  Patcher.setImageContext(&*Image);
  auto Installed =
      Patcher.patch(Input, Output, *Lifted.LlvmModule, Image->Arch);
  ASSERT_TRUE(Installed.Success);
  ASSERT_EQ(Installed.PatchedFunctions.size(), 1u);
  const auto &Entry = Installed.PatchedFunctions.front();
  ASSERT_EQ(Entry.OriginalVA, Platform->Entry);
  ASSERT_NE(Entry.OwnerVA, Entry.OriginalVA);
  auto Unchanged = Reader->load(Input);
  ASSERT_TRUE(bool(Unchanged)) << llvm::toString(Unchanged.takeError());
  ASSERT_EQ(Unchanged->Raw, Image->Raw);
  testing::Test::RecordProperty(
      kPlatformHash, llvm::toHex(llvm::SHA256::hash(Platform->Raw), true));
  testing::Test::RecordProperty(
      kUnsignedHash, llvm::toHex(llvm::SHA256::hash(Image->Raw), true));
  testing::Test::RecordProperty(kEntryVA, std::to_string(Entry.OriginalVA));
  testing::Test::RecordProperty(kOwnerVA, std::to_string(Entry.OwnerVA));
  for (const auto &Path : {Input, Output}) {
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  kCodesign, {kCodesign, kSign, kAdHocIdentity, Path}),
              0);
    ASSERT_EQ(llvm::sys::ExecuteAndWait(kCodesign,
                                        {kCodesign, kVerify, kStrict, Path}),
              0);
    auto Signed = Reader->load(Path);
    ASSERT_TRUE(bool(Signed)) << llvm::toString(Signed.takeError());
    testing::Test::RecordProperty(
        Path == Input ? kOriginalHash : kPublishedHash,
        llvm::toHex(llvm::SHA256::hash(Signed->Raw), true));
    if (std::getenv(kExecute)) {
      std::filesystem::permissions(Path, std::filesystem::perms::owner_exec,
                                   std::filesystem::perm_options::add);
      ASSERT_EQ(llvm::sys::ExecuteAndWait(Path, {Path}), ExitCode);
    }
  }
  if (std::getenv(kExecute)) {
    testing::Test::RecordProperty(kExitCode, ExitCode);
    testing::Test::RecordProperty(kNativeImages, kCount);
  }
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64DarwinExecutable, PublishesDarwinTrue) {
  verifyPublishedDarwinExecutable(kDarwinTrue, EXIT_SUCCESS);
}

TEST(AArch64DarwinExecutable, PublishesDarwinFalse) {
  verifyPublishedDarwinExecutable(kDarwinFalse, EXIT_FAILURE);
}

TEST(AArch64LiftedImage, VerifiedLeafBodiesUseTheNormalPatchPipeline) {
  verifyPublishedLeaves(kLeaves, kCaller);
}

TEST(AArch64LiftedImage, DirectTailTransferPreservesItsInstalledCallee) {
  verifyPublishedLeaves(kTailLeaves, kCaller, kAssemblyName, kTailSum, true);
}

TEST(AArch64LiftedImage, EquivalentWAndXZeroingPreservesTheWholeRegister) {
  verifyPublishedLeaves(kZeroLeaves, kCaller, kAssemblyName, kZeroResults);
}

TEST(AArch64DarwinEntry, SwiftContextSurvivesNativeLeafPublication) {
  verifyPublishedLeaves(kSwiftLeaves, kSwiftCaller, kSource, kSwiftABI);
}

TEST(AArch64DarwinEntry, SwiftAsyncContextSurvivesNativeLeafPublication) {
  verifyPublishedLeaves(kSwiftLeaves, kSwiftCaller, kSource, kSwiftAsyncABI);
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
