//===- AArch64LiftedLeafTests.cpp - Native lifted leaf publication --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/llvm/AArch64/LLVMAArch64ReturnProtection.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/loader/Loader.h"
#include "neverd/object/SectionNames.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <array>
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

enum class LeafCheck {
  None,
  TailTarget,
  StackArgument,
  RestoringLoad,
  ErrorResult,
  Global,
  FramedImport,
  FramedError,
  IndexedError,
  AsyncIndexedError
};

void verifyPublishedLeaves(const char *Leaves, const char *CallerSource,
                           const char *SourceName = kSource,
                           std::string_view Preamble = {},
                           LeafCheck Check = LeafCheck::None) {
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
  const bool KeepFrame =
      Check == LeafCheck::FramedImport || Check == LeafCheck::FramedError;
  llvm::SmallVector<llvm::StringRef> Compile{
      kCompiler,
      kTargetFlag,
      kTarget,
      kOptimize,
      KeepFrame ? kKeepFrame : kOmitFrame,
      kDylib,
      kTight};
  const bool Async = Check == LeafCheck::AsyncIndexedError;
  const bool Indexed = Check == LeafCheck::IndexedError || Async;
  if (Async)
    Compile.push_back(kAsyncTables);
  Compile.append({Source, kOut, Input});
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

  if (Check == LeafCheck::ErrorResult || Check == LeafCheck::FramedError ||
      Indexed) {
    auto MissingWrite = llvm::CloneModule(Module);
    auto *Writer = MissingWrite->getFunction(kDifference);
    ASSERT_NE(Writer, nullptr);
    llvm::SmallVector<llvm::IntrinsicInst *> Writes;
    for (auto &BB : *Writer)
      for (auto &I : BB)
        if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
            Call && Call->getIntrinsicID() == llvm::Intrinsic::write_register)
          Writes.push_back(Call);
    ASSERT_EQ(Writes.size(), 1u);
    Writes.front()->eraseFromParent();
    EXPECT_FALSE(
        Patcher.patch(Input, Rejected, *MissingWrite, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }
  if (Indexed || Check == LeafCheck::RestoringLoad) {
    auto Bytes = Snapshot;
    if (Indexed) {
      const auto *FrameSection =
          Image->getSectionByName(section_names::macho::EhFrame);
      ASSERT_NE(FrameSection, nullptr);
      auto Frame = llvm::MutableArrayRef(Bytes).slice(FrameSection->FileOff,
                                                      FrameSection->FileSz);
      const auto Find = [&](auto Begin) {
        return std::search(Begin, Frame.end(), kLinkSpillRule.begin(),
                           kLinkSpillRule.end());
      };
      auto At = Find(Frame.begin());
      ASSERT_NE(At, Frame.end());
      ASSERT_EQ(Find(At + kLinkSpillRule.size()), Frame.end());
      At[kLinkSpillRule.size() - 1] = kWrongLinkOffsetFactor;
    } else {
      // Changing pre-indexing to post-indexing restores the same SP but reads
      // from the private frame. Check the published candidate against those
      // actual input bytes as well.
      const auto *Text = Image->findSectionByName(section_names::macho::Text);
      ASSERT_NE(Text, nullptr);
      unsigned Restores = 0;
      for (size_t Off = 0; Off + aarch64::kInsnSize <= Text->FileSz;
           Off += aarch64::kInsnSize) {
        auto *At = Bytes.data() + Text->FileOff + Off;
        const auto Word = readLE<uint32_t>(At);
        const auto Update = aarch64WritebackMemoryInstruction(Word);
        if (Update && Update->isLoad() && Update->stackBase() &&
            Update->preIndex()) {
          writeLE<uint32_t>(At, Word & ~kWritebackPreIndexBit);
          ++Restores;
        }
      }
      ASSERT_EQ(Restores, 1u);
    }
    const auto Invalid =
        (Root / (Indexed ? kWrongSpillInput : kPostIndexedInput)).string();
    std::ofstream(Invalid, std::ios::binary)
        .write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    ASSERT_EQ(
        llvm::sys::ExecuteAndWait(
            kCodesign, {kCodesign, kForceSign, kSign, kAdHocIdentity, Invalid}),
        0);
    auto Altered = Reader->load(Invalid);
    ASSERT_TRUE(bool(Altered)) << llvm::toString(Altered.takeError());
    if (Indexed) {
      ASSERT_TRUE(llvm::any_of(
          Altered->ExceptionMetadata.Functions, [&](const auto &F) {
            return F.Dwarf &&
                   llvm::any_of(F.Dwarf->Instructions, [](const auto &I) {
                     return I.Kind == CFIOpKind::Offset &&
                            I.Offset == -int64_t(sizeof(uint64_t));
                   });
          }));
    }
    MachOPatcher AlteredPatcher;
    AlteredPatcher.setImageContext(&*Altered);
    EXPECT_FALSE(
        AlteredPatcher.patch(Invalid, Rejected, Module, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }
  if (Check == LeafCheck::StackArgument || Check == LeafCheck::RestoringLoad) {
    // Redirect the second incoming word to the preceding fixed carrier. The
    // native instruction still has the same load width and destination.
    auto Shifted = llvm::CloneModule(Module);
    auto *F = Shifted->getFunction(kDifference);
    ASSERT_NE(F, nullptr);
    ASSERT_GE(F->arg_size(), kCount);
    auto *Last = F->getArg(F->arg_size() - 1);
    ASSERT_FALSE(Last->use_empty());
    Last->replaceAllUsesWith(F->getArg(F->arg_size() - kCount));
    EXPECT_FALSE(Patcher.patch(Input, Rejected, *Shifted, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }
  if (Check == LeafCheck::FramedImport) {
    auto Retargeted = llvm::CloneModule(Module);
    auto *Import = Retargeted->getFunction(kProcessImport);
    auto *Read = Retargeted->getFunction(kSum);
    ASSERT_NE(Import, nullptr);
    ASSERT_EQ(Import->getFunctionType(), Read->getFunctionType());
    Import->replaceAllUsesWith(Read);
    EXPECT_FALSE(
        Patcher.patch(Input, Rejected, *Retargeted, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }
  if (Check == LeafCheck::Global || Check == LeafCheck::FramedImport) {
    auto Redirected = llvm::CloneModule(Module);
    auto *Read = Redirected->getFunction(kSum);
    auto *Ret = llvm::cast<llvm::ReturnInst>(Read->back().getTerminator());
    auto *Load = llvm::dyn_cast<llvm::LoadInst>(Ret->getReturnValue());
    ASSERT_NE(Load, nullptr);
    llvm::IRBuilder<> Builder(Load);
    Load->setOperand(0, Builder.CreateConstGEP1_64(
                            Load->getType(), Load->getPointerOperand(), 1));
    EXPECT_FALSE(
        Patcher.patch(Input, Rejected, *Redirected, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }
  const auto ReorderedPath = (Root / kReorderedOutput).string();
  if (Check == LeafCheck::TailTarget)
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
    if (Indexed && Entry.SourceFunction == kSum) {
      for (const auto &[View, VA] : {std::pair{&*Image, Entry.OriginalVA},
                                     std::pair{&*Final, Entry.OwnerVA}}) {
        const auto *F = View->ExceptionMetadata.findFunction(VA);
        ASSERT_NE(F, nullptr);
        ASSERT_TRUE(F->Dwarf);
        const auto *C =
            View->ExceptionMetadata.findCIE(F->Dwarf->CIESectionOffset);
        ASSERT_NE(C, nullptr);
        const auto Restored =
            llvm::find_if(F->Dwarf->Instructions, [&](const auto &I) {
              return I.Kind == CFIOpKind::Restore &&
                     I.Register == C->ReturnAddressRegister;
            });
        ASSERT_EQ(Restored != F->Dwarf->Instructions.end(), Async);
        if (Async) {
          EXPECT_EQ(Restored->CodeOffset + aarch64::kInsnSize,
                    F->Dwarf->AddressRange);
          testing::Test::RecordProperty(kRestoreBoundary,
                                        std::to_string(Restored->CodeOffset));
        }
      }
    }
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
    if (Check == LeafCheck::TailTarget) {
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

// An independent variadic caller exercises the public machine entry. Read
// the input as arm64e even on developer machines booted for ordinary arm64.
void verifyVariadicEntry(const char *Optimization) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kTemporary, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kSource).string();
  const auto Input = (Root / kOriginal).string();
  const auto Control = (Root / kControl).string();
  const auto Caller = (Root / kDriverSource).string();
  const auto Driver = (Root / kDriver).string();
  const bool Native = std::getenv(kExecute);
  const char *Target = Native ? kTarget : kHostTarget;
  std::ofstream(Source) << kVariadicLeaves;
  std::ofstream(Caller) << kExtendedVariadics << kVariadicCaller;
  auto Library = [&](const std::string &File, const char *Triple,
                     const std::string &Path) {
    return llvm::sys::ExecuteAndWait(kCompiler, {kCompiler, kTargetFlag, Triple,
                                                 Optimization, kOmitFrame,
                                                 kDylib, File, kOut, Path});
  };
  ASSERT_EQ(Library(Source, kTarget, Input), 0);
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(kCompiler, {kCompiler, kTargetFlag, Target,
                                            kOptimize, Caller, kOut, Driver}),
      0);
  if (!Native)
    ASSERT_EQ(Library(Source, Target, Control), 0);
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(Driver, {Driver, Native ? Input : Control}), 0);

  auto Image = Loader::create(BinaryFormat::MachO)->load(Input);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->isArm64e());
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.LiftMode = true;
  Options.EmitDumpOutput = false;
  auto Lifted = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Lifted.Success) << Lifted.Error;
  ASSERT_TRUE(Lifted.LlvmModule);
  auto &Module = *Lifted.LlvmModule;
  ASSERT_TRUE(Module.getModuleFlag(kArm64eAnalysisModuleFlag));
  auto *Walk = Module.getFunction(kDifference);
  ASSERT_NE(Walk, nullptr);
  ASSERT_TRUE(Walk->isVarArg());
  ASSERT_TRUE(Walk->hasFnAttribute(llvm::Attribute::NoInline));
  EXPECT_TRUE(llvm::any_of(*Walk, [](const auto &Block) {
    return llvm::any_of(Block, [](const auto &I) {
      const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
      return Call && Call->getIntrinsicID() == llvm::Intrinsic::sponentry;
    });
  }));
  // Apple Clang requires its frontend's classic ABI flag for arm64e IR.
  // The analysis marker stays attached throughout this compiler control.
  // Older system Clang IR readers lack LLVM's target_mem refinement. Removing
  // this optional optimizer promise preserves the function's instructions.
  for (auto &F : Module)
    F.removeFnAttr(llvm::Attribute::Memory);
  // A stack protector moves the cursor's synthetic local away from the
  // caller. An out-of-bounds local load can otherwise work by coincidence.
  for (auto &F : Module)
    if (!F.isDeclaration())
      F.addFnAttr(llvm::Attribute::StackProtectReq);
  Module.setTargetTriple(llvm::Triple(Target));
  if (Native) {
    llvm::IRBuilder<> B(Context);
    auto *ABI = llvm::MDNode::get(
        Context, {llvm::ConstantAsMetadata::get(B.getInt32(kClassicABIVersion)),
                  llvm::ConstantAsMetadata::get(B.getFalse())});
    Module.addModuleFlag(llvm::Module::AppendUnique, kClassicABI,
                         llvm::MDNode::get(Context, {ABI}));
  }
  const auto IR = (Root / kLiftedIR).string();
  const auto C = (Root / kLiftedC).string();
  std::error_code Error;
  llvm::raw_fd_ostream Stream(IR, Error);
  ASSERT_FALSE(Error);
  Module.print(Stream, nullptr);
  Stream.close();
  ASSERT_FALSE(Stream.has_error());
  CEmitterOptions COptions;
  COptions.PreserveLLVMFunctionTypes = true;
  COptions.TheArch = Image->Arch;
  COptions.Format = Image->Format;
  ASSERT_TRUE(LLVMCEmitter().emitToFile(Module, C, COptions, nullptr, &*Image));
  Options.LiftMode = false;
  auto Structured = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Structured.Success) << Structured.Error;
  ASSERT_FALSE(Structured.HighFuncs.empty());
  const auto HighC = (Root / kStructuredC).string();
  COptions.PreserveLLVMFunctionTypes = false;
  COptions.Image = &*Image;
  ASSERT_TRUE(HighCEmitter().emitToFile(Structured.HighFuncs, HighC, COptions));
  const auto Output = (Root / kOutput).string();
  const std::array Sources{IR, C, HighC};
  for (const auto &File : Sources) {
    SCOPED_TRACE(File);
    ASSERT_EQ(Library(File, Target, Output), 0);
    ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, Output}), 0);
  }
  if (Native)
    testing::Test::RecordProperty(kNativeImages,
                                  static_cast<int>(Sources.size() + 1));
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64DarwinVariadic, OptimizedCalleeWalksTheNativeArgumentArea) {
  verifyVariadicEntry(kOptimize);
}

TEST(AArch64DarwinVariadic, UnoptimizedCalleeHomesItsNativeArgumentCursor) {
  verifyVariadicEntry(kUnoptimized);
}

TEST(AArch64DarwinVariadic, FramelessStackLoadsSurviveImagePublication) {
  verifyPublishedLeaves(kStackLoadLeaves, kVariadicCaller, kAssemblyName, {},
                        LeafCheck::StackArgument);
}

TEST(AArch64DarwinVariadic, CompactLeavesElideOnlyTheUnusedStackReservation) {
  verifyPublishedLeaves(kReservedStackLeaves, kVariadicCaller, kSource,
                        kReservedFrame, LeafCheck::StackArgument);
}

TEST(AArch64DarwinVariadic, PreIndexedArgumentLoadRestoresTheIncomingSP) {
  verifyPublishedLeaves(kReservedStackLeaves, kVariadicCaller, kSource,
                        kRestoreOnLoad, LeafCheck::RestoringLoad);
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
  verifyPublishedLeaves(kTailLeaves, kCaller, kAssemblyName, kTailSum,
                        LeafCheck::TailTarget);
}

TEST(AArch64LiftedImage, EquivalentWAndXZeroingPreservesTheWholeRegister) {
  verifyPublishedLeaves(kZeroLeaves, kCaller, kAssemblyName, kZeroResults);
}

TEST(AArch64DarwinEntry,
     SwiftErrorReturnsBothTheOldValueAndTheUpdatedRegister) {
  verifyPublishedLeaves(kErrorLeaves, kErrorCaller, kSource, kErrorABI,
                        LeafCheck::ErrorResult);
}

TEST(AArch64DarwinEntry, SwiftErrorCanClearTheRegisterWithoutReadingIt) {
  verifyPublishedLeaves(kErrorLeaves, kErrorCaller, kSource,
                        std::string(kErrorABI) + kClearError,
                        LeafCheck::ErrorResult);
}

TEST(AArch64DarwinEntry, TailCallForwardsTheWritableSwiftErrorRegister) {
  verifyPublishedLeaves(kTailErrorLeaves, kErrorCaller, kAssemblyName,
                        std::string(kErrorABI) + kForwardError,
                        LeafCheck::ErrorResult);
}

TEST(AArch64DarwinEntry, SignedCallReturnsTheUpdatedSwiftErrorRegister) {
  verifyPublishedLeaves(kErrorLeaves, kErrorCaller, kSource,
                        std::string(kErrorABI) + kForwardError,
                        LeafCheck::FramedError);
}

TEST(AArch64DarwinEntry, IndexedDwarfCallReturnsTheUpdatedSwiftErrorRegister) {
  verifyPublishedLeaves(kErrorLeaves, kErrorCaller, kSource,
                        std::string(kErrorABI) + kForwardError,
                        LeafCheck::IndexedError);
}

TEST(AArch64DarwinEntry, AsyncDwarfRestoresTheLinkRegisterBeforeSwiftReturn) {
  verifyPublishedLeaves(kErrorLeaves, kErrorCaller, kSource,
                        std::string(kErrorABI) + kForwardError,
                        LeafCheck::AsyncIndexedError);
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

TEST(AArch64LiftedImage,
     OriginalGlobalAndAuthenticatedImportSurvivePublication) {
  verifyPublishedLeaves(kGlobalLeaves, kGlobalCaller, kSource, {},
                        LeafCheck::Global);
}
TEST(AArch64LiftedImage, SignedFrameCallsImportBeforeReadingOriginalGlobal) {
  verifyPublishedLeaves(kGlobalLeaves, kGlobalCaller, kSource, kFramedImport,
                        LeafCheck::FramedImport);
}
} // namespace
