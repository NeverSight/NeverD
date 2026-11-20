//===- AArch64LiftedLeafTests.cpp - Native lifted leaf publication --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/MachO/MachORegisterResults.h"
#include "neverd/backend/codegen/MachO/MachOSignaturePolicy.h"
#include "neverd/backend/codegen/MachO/MachOSigner.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/llvm/AArch64/LLVMAArch64ReturnProtection.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/object/SectionNames.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
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
#include <memory>
#include <string_view>
#include <utility>

namespace {
using namespace neverd;
#define A64_LEAF_TEXT(Name, Text) constexpr char Name[] = Text;
#define A64_LEAF_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64LiftedLeafFixtures.def"
#define A64_LEAF_TEXT(Name, Text) constexpr char Name[] = Text;
#include "AArch64LiftedSIMDFixtures.def"

#if defined(__APPLE__) && defined(__aarch64__)
void verifyTailTargets(llvm::Module &Module, MachOPatcher &Patcher,
                       const std::string &Input, const std::string &Rejected,
                       const std::string &ReorderedPath, Arch Architecture) {
  auto Candidate = llvm::CloneModule(Module);
  auto *Forwarder = Candidate->getFunction(kDifference);
  auto *Callee = Candidate->getFunction(kSum);
  ASSERT_NE(Forwarder, nullptr);
  ASSERT_NE(Callee, nullptr);
  llvm::Use *Target = nullptr;
  if (Forwarder->hasFnAttribute(llvm::Attribute::Naked)) {
    auto *Call = llvm::dyn_cast<llvm::CallInst>(&Forwarder->front().front());
    ASSERT_TRUE(Call && Call->isInlineAsm());
    ASSERT_EQ(Call->arg_size(), 1u);
    Target = &Call->getArgOperandUse(0);
  } else {
    auto *Return =
        llvm::dyn_cast<llvm::ReturnInst>(Forwarder->back().getTerminator());
    ASSERT_NE(Return, nullptr);
    auto *Call =
        llvm::dyn_cast_or_null<llvm::CallInst>(Return->getReturnValue());
    ASSERT_NE(Call, nullptr);
    ASSERT_TRUE(Call->isTailCall());
    Target = &Call->getCalledOperandUse();
  }
  ASSERT_EQ(Target->get(), Callee);
  Target->set(Forwarder);
  EXPECT_FALSE(
      Patcher.patch(Input, Rejected, *Candidate, Architecture).Success);
  EXPECT_FALSE(std::filesystem::exists(Rejected));

  Target->set(Callee);
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
  ScratchResult,
  KilledScratchResult,
  NativeAddressing,
  NativeSIMD,
  FrameScratchResult,
  ErrorResult,
  Global,
  FramedImport,
  FramedError,
  IndexedError,
  AsyncIndexedError,
  Resigned,
  Relifted,
  SigningRequirements
};

void verifyPublishedLeaves(const char *Leaves, const char *CallerSource,
                           const char *SourceName = kSource,
                           std::string_view Preamble = {},
                           LeafCheck Check = LeafCheck::None,
                           unsigned ExpectedEntries = kCount) {
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
  const bool ScratchFrame = Check == LeafCheck::FrameScratchResult;
  const bool Embedded = Preamble == kMixedOracle || Preamble == kDispatchOracle;
  if (Embedded)
    Compile.push_back(kDataLinker);
  if (Async)
    Compile.push_back(kAsyncTables);
  Compile.append({Source, kOut, Input});
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, Compile), 0);
  if (Embedded) {
    // The integrated assembler emits both instruction flags for __text.
    // Exercise the ABI's SOME-only spelling with the same linked DICE records.
    auto File = llvm::MemoryBuffer::getFile(Input);
    ASSERT_TRUE(bool(File));
    auto Buffer = (*File)->getBuffer();
    std::vector<uint8_t> Bytes(Buffer.bytes_begin(), Buffer.bytes_end());
    bool Found = false;
    forEachMachOLoadCommand(
        Bytes.data(), Bytes.size(),
        [&](const uint8_t *P, uint32_t ID, uint32_t, bool) {
          if (ID != llvm::MachO::LC_SEGMENT_64)
            return;
          const auto Segment = readMachOSegment(P, true);
          auto *S = reinterpret_cast<llvm::MachO::section_64 *>(
              Bytes.data() + (P - Bytes.data()) +
              sizeof(llvm::MachO::segment_command_64));
          for (unsigned I = 0; I < Segment.NSects; ++I)
            if (readMachOName(S[I].sectname) == section_names::macho::Text) {
              S[I].flags &= ~llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
              Found = true;
            }
        });
    ASSERT_TRUE(Found);
    std::ofstream(Input, std::ios::binary)
        .write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  }
  if (Check == LeafCheck::Resigned || Check == LeafCheck::Relifted ||
      Check == LeafCheck::SigningRequirements) {
    llvm::SmallVector<llvm::StringRef> Sign{kCodesign,       kForceSign,
                                            kSign,           kAdHocIdentity,
                                            kSignIdentifier, kRepeatIdentity};
    if (Check == LeafCheck::SigningRequirements)
      Sign.append({kSignRequirements, kNonemptyRequirements});
    Sign.push_back(Input);
    ASSERT_EQ(llvm::sys::ExecuteAndWait(kCodesign, Sign), 0);
    ASSERT_EQ(llvm::sys::ExecuteAndWait(kCodesign,
                                        {kCodesign, kVerify, kStrict, Input}),
              0);
  }
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Image = Reader->load(Input);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  if (Check == LeafCheck::Relifted) {
    std::vector<ExceptionAddressRange> Sections;
    for (const auto &S : Image->Sections)
      if (S.Size && (S.Type & llvm::MachO::S_ATTR_PURE_INSTRUCTIONS) &&
          (S.Type & llvm::MachO::SECTION_TYPE) == llvm::MachO::S_REGULAR)
        Sections.push_back({S.VA, S.VA + S.Size});
    llvm::sort(Sections,
               [](const auto &L, const auto &R) { return L.Begin < R.Begin; });
    testing::Test::RecordProperty(kSourceCodeSections, int(Sections.size()));
    const bool Separate = std::adjacent_find(Sections.begin(), Sections.end(),
                                             [](const auto &L, const auto &R) {
                                               return L.End < R.Begin;
                                             }) != Sections.end();
    testing::Test::RecordProperty(kSeparatedCodeSections, int(Separate));
    const auto Distinct = std::adjacent_find(
        Sections.begin(), Sections.end(), [&](const auto &L, const auto &R) {
          return Image->getSegmentFor(L.Begin) != Image->getSegmentFor(R.Begin);
        });
    testing::Test::RecordProperty(kDistinctCodeSegments,
                                  int(Distinct != Sections.end()));
  }
  if (Preamble == kSplitData || Preamble == kSplitSegments) {
    const auto *Marker = Image->findSymbol(kMarkerSymbol);
    ASSERT_TRUE(Marker);
    EXPECT_FALSE(Image->isCodeAddress(Marker->Addr));
    EXPECT_FALSE(Image->hasAuthenticatedFunctionEntryAt(Marker->Addr));
  }
  if (Embedded) {
    const uint16_t Kinds[] = {llvm::MachO::DICE_KIND_DATA,
                              llvm::MachO::DICE_KIND_JUMP_TABLE8,
                              llvm::MachO::DICE_KIND_JUMP_TABLE16,
                              llvm::MachO::DICE_KIND_JUMP_TABLE32};
    ASSERT_EQ(Image->MachODataInCode.size(), std::size(Kinds));
    for (const auto &[I, R] : llvm::enumerate(Image->MachODataInCode)) {
      EXPECT_EQ(R.Kind, Kinds[I]);
      for (va_t VA = R.Begin; VA < R.End; ++VA) {
        EXPECT_FALSE(Image->isCodeAddress(VA));
        EXPECT_FALSE(Image->hasExecutableCodeOwnerAt(VA));
        EXPECT_FALSE(Image->hasAuthenticatedFunctionEntryAt(VA));
      }
    }
    const auto PIE = (Root / kPrefixControl).string();
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  kCompiler, {kCompiler, kTargetFlag, kTarget, kDataLinker,
                              kMixedEntry, Source, kOut, PIE}),
              0);
    auto Before = Reader->load(PIE);
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    ASSERT_EQ(Before->MachODataInCode.size(), std::size(Kinds));
    auto Prefix = Before->Raw;
    auto Grew = reserveMachOHeaderStorage(
        Prefix, Arch::AArch64, macho_loader::arm64e::kLargePageBytes);
    ASSERT_TRUE(bool(Grew)) << llvm::toString(Grew.takeError());
    ASSERT_TRUE(*Grew);
    auto After = Reader->loadBuffer(llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(Prefix.data()),
                        Prefix.size()),
        PIE));
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
    ASSERT_EQ(Before->MachODataInCode, After->MachODataInCode);
    EXPECT_LT(After->fileOffsetToVA(0), Before->fileOffsetToVA(0));
    EXPECT_EQ(Before->Entry, After->Entry);
    for (const auto &R : Before->MachODataInCode) {
      const auto Size = R.End - R.Begin;
      EXPECT_GT(After->vaToFileOffset(R.Begin),
                Before->vaToFileOffset(R.Begin));
      EXPECT_EQ(llvm::ArrayRef(Before->readVA(R.Begin, Size), Size),
                llvm::ArrayRef(After->readVA(R.Begin, Size), Size));
    }
    testing::Test::RecordProperty(kPrefixRecords,
                                  int(After->MachODataInCode.size()));
    const uint8_t *Command = nullptr;
    forEachMachOLoadCommand(Image->Raw.data(), Image->Raw.size(),
                            [&](const uint8_t *P, uint32_t ID, uint32_t, bool) {
                              if (ID == llvm::MachO::LC_DATA_IN_CODE)
                                Command = P;
                            });
    ASSERT_TRUE(Command);
    const auto &Table =
        *reinterpret_cast<const llvm::MachO::linkedit_data_command *>(Command);
    using Record = llvm::MachO::data_in_code_entry;
    const auto InvalidTable = [&](auto Modify) {
      auto Bytes = Image->Raw;
      auto &D = *reinterpret_cast<llvm::MachO::linkedit_data_command *>(
          Bytes.data() + (Command - Image->Raw.data()));
      auto *R = reinterpret_cast<Record *>(Bytes.data() + Table.dataoff);
      Modify(D, R);
      auto Invalid = Reader->loadBuffer(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                          Bytes.size()),
          Input));
      EXPECT_FALSE(bool(Invalid));
      if (!Invalid)
        llvm::consumeError(Invalid.takeError());
    };
    InvalidTable([](auto &D, Record *) { --D.datasize; });
    InvalidTable([](auto &, Record *R) { R->length = 0; });
    InvalidTable([](auto &, Record *R) { R[1].offset = R[0].offset; });
    InvalidTable([](auto &, Record *R) { R->offset = UINT32_MAX; });
    auto Island = Image->MachODataInCode.front();
    EXPECT_FALSE(
        Image->isCodeRange(Island.Begin - aarch64::kInsnSize,
                           Island.End - Island.Begin + aarch64::kInsnSize));
    EXPECT_FALSE(Image->hasExecutableCodeOwnerRange(
        Island.Begin - aarch64::kInsnSize,
        Island.End - Island.Begin + aarch64::kInsnSize));
    const auto *Section = Image->getSectionFor(Island.Begin);
    ASSERT_TRUE(Section);
    EXPECT_TRUE(Section->Type & llvm::MachO::S_ATTR_SOME_INSTRUCTIONS);
    EXPECT_FALSE(Section->Type & llvm::MachO::S_ATTR_PURE_INSTRUCTIONS);
  }
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

  if (Embedded) {
    auto *F = Module.getFunction(kSum);
    ASSERT_TRUE(F && F->hasFnAttribute(llvm::Attribute::Naked));
    const auto *Call = llvm::dyn_cast<llvm::CallInst>(&F->front().front());
    ASSERT_TRUE(Call && Call->isInlineAsm());
    const auto *Asm = llvm::cast<llvm::InlineAsm>(Call->getCalledOperand());
    EXPECT_TRUE(Asm->getAsmString().contains(kRegionAssembly));
  }
  if (Preamble == kDispatchOracle) {
    unsigned Proved = 0;
    for (const auto &Function : Lifted.LowFuncs) {
      ASSERT_EQ(Function.JumpTables.size(), 1u);
      const auto &Table = Function.JumpTables.front();
      EXPECT_TRUE(Table.IndexDomainAuthenticated);
      EXPECT_EQ(Table.Targets.size(), kDispatchSlots);
      ++Proved;
    }
    ASSERT_EQ(Proved, kDispatchFunctions);
    testing::Test::RecordProperty(kDispatchGuards, Proved);

    const auto ByteFunction =
        llvm::find_if(Lifted.LowFuncs, [](const LowFunc &F) {
          return F.JumpTables.front().EntrySize == sizeof(uint8_t);
        });
    ASSERT_NE(ByteFunction, Lifted.LowFuncs.end());
    const auto &Table = ByteFunction->JumpTables.front();
    EXPECT_NE(Table.BaseAddr % aarch64::kInsnSize, 0u);
    // A tight Mach-O can append its new body to __TEXT instead of creating
    // a sectionless segment. Other declared sections still bound that segment.
    auto Sectionless = *Image;
    const auto *Section = Sectionless.getSectionFor(Table.BaseAddr);
    ASSERT_TRUE(Section);
    const auto TextVA = Section->VA;
    llvm::erase_if(Sectionless.Sections,
                   [&](const auto &S) { return S.VA == TextVA; });
    const auto *Segment = Sectionless.getSegmentFor(Table.BaseAddr);
    ASSERT_TRUE(Segment &&
                Sectionless.segmentHasReadableSectionMetadata(*Segment));
    for (const auto &R : Image->MachODataInCode) {
      EXPECT_EQ(Sectionless.mappedObjectOwnerEnd(R.Begin), R.End);
      EXPECT_EQ(Sectionless.mappedObjectOwnerEnd(R.End - 1), R.End);
    }
    EXPECT_FALSE(Sectionless.mappedObjectOwnerEnd(ByteFunction->Entry));
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Sectionless));
    for (const auto &Function : Lifted.LowFuncs) {
      auto Recovered = CFGBuilder().build(Sectionless, Decode, Function.Entry);
      ASSERT_EQ(Recovered.JumpTables.size(), 1u);
      EXPECT_TRUE(Recovered.JumpTables.front().IndexDomainAuthenticated);
      EXPECT_EQ(Recovered.JumpTables.front().Targets,
                Function.JumpTables.front().Targets);
    }
    const auto Entry = ByteFunction->Entry;
    const auto Guard = Image->vaToFileOffset(Entry + aarch64::kInsnSize);
    const auto Anchor = Image->vaToFileOffset(
        Table.InsnAddr - kDispatchAnchorDistance * aarch64::kInsnSize);
    const auto FirstSlot = Image->vaToFileOffset(Table.BaseAddr);
    const auto Reject = [&](auto Change) {
      auto Bytes = Image->Raw;
      Change(Bytes);
      auto Candidate = Reader->loadBuffer(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                          Bytes.size()),
          Input));
      ASSERT_TRUE(bool(Candidate)) << llvm::toString(Candidate.takeError());
      auto Valid = validateMachORegisterResults(Module, *Candidate);
      EXPECT_FALSE(bool(Valid));
      if (!Valid)
        llvm::consumeError(Valid.takeError());
      llvm::LLVMContext FreshContext;
      auto Fresh = Pipeline().run(*Candidate, FreshContext, Options);
      if (Fresh.Success && Fresh.LlvmModule)
        if (auto *F = Fresh.LlvmModule->getFunction(kSum))
          EXPECT_FALSE(F->hasFnAttribute(llvm::Attribute::Naked));
    };
    // An equality exit does not bound the index above. The old module
    // cannot grant the candidate its original control-flow proof.
    Reject([&](auto &B) {
      const auto Branch = readLE<uint32_t>(B.data() + Guard);
      writeLE<uint32_t>(B.data() + Guard, Branch & ~kDispatchChangeCondition);
    });
    // The scalar and ADR agree at the original VA, but only ADR follows the
    // new body. This dylib's preferred anchor fits the single MOVZ immediate.
    ASSERT_LE(Table.TargetBase, UINT16_MAX);
    Reject([&](auto &B) {
      writeLE<uint32_t>(B.data() + Anchor,
                        kDispatchMoveX11 |
                            (Table.TargetBase << kDispatchMoveImmShift));
    });
    // The first slot would now dispatch into table data, with its other
    // slots and its selector bound unchanged.
    const auto Literal = Image->MachODataInCode.front().Begin;
    ASSERT_LT(Table.TargetBase, Literal);
    const auto ToData = Literal - Table.TargetBase;
    ASSERT_EQ(ToData % aarch64::kInsnSize, 0u);
    ASSERT_LE(ToData / aarch64::kInsnSize, UINT8_MAX);
    Reject([&](auto &B) { B[FirstSlot] = ToData / aarch64::kInsnSize; });
    // The file still has exactly the same instructions and slots, but its
    // declared entry width no longer agrees with the actual LDRB.
    Reject([&](auto &B) {
      unsigned ChangedKinds = 0;
      forEachMachOLoadCommand(
          B.data(), B.size(),
          [&](const uint8_t *P, uint32_t ID, uint32_t, bool) {
            if (ID != llvm::MachO::LC_DATA_IN_CODE)
              return;
            const auto &Command =
                *reinterpret_cast<const llvm::MachO::linkedit_data_command *>(
                    P);
            auto *Slots = reinterpret_cast<llvm::MachO::data_in_code_entry *>(
                B.data() + Command.dataoff);
            for (auto &Slot : llvm::MutableArrayRef(Slots, Command.datasize /
                                                               sizeof(*Slots)))
              if (Slot.kind == llvm::MachO::DICE_KIND_JUMP_TABLE8) {
                Slot.kind = llvm::MachO::DICE_KIND_JUMP_TABLE16;
                ++ChangedKinds;
              }
          });
      EXPECT_EQ(ChangedKinds, 1u);
    });
    testing::Test::RecordProperty(kDispatchNegatives, kDispatchRejections);
  }
  if (Check == LeafCheck::KilledScratchResult) {
    for (const auto *Name : {kSum, kDifference}) {
      auto *Entry = Module.getFunction(Name);
      ASSERT_NE(Entry, nullptr);
      EXPECT_FALSE(Entry->hasFnAttribute(llvm::Attribute::Naked));
    }
    testing::Test::RecordProperty(kNativeImages, 0);
    return;
  }

  MachOPatcher Patcher;
  Patcher.setImageContext(&*Image);
  if (Check == LeafCheck::SigningRequirements) {
    auto Signature = macho_signature::inspect(Image->Raw);
    ASSERT_TRUE(bool(Signature)) << llvm::toString(Signature.takeError());
    EXPECT_FALSE(macho_signature::canTransactionallyAdHocResign(*Signature));
    EXPECT_FALSE(Patcher.patch(Input, Output, Module, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Output));
    auto Original = Reader->load(Input);
    ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
    EXPECT_EQ(Original->Raw, Snapshot);
    testing::Test::RecordProperty(kRejectedRequirements, 1);
    testing::Test::RecordProperty(kNativeImages, 0);
    return;
  }
  auto Partial = llvm::CloneModule(Module);
  ASSERT_NE(Partial->getFunction(kDifference), nullptr);
  Partial->getFunction(kDifference)->deleteBody();
  EXPECT_FALSE(Patcher.patch(Input, Rejected, *Partial, Image->Arch).Success);
  EXPECT_FALSE(std::filesystem::exists(Rejected));

  auto Changed = llvm::CloneModule(Module);
  auto *Sum = Changed->getFunction(kSum);
  ASSERT_NE(Sum, nullptr);
  auto *Result = Sum->getReturnType();
  auto *Wrong =
      Result->isVoidTy() ? nullptr : llvm::Constant::getNullValue(Result);
  if (Result->isIntegerTy())
    Wrong = llvm::ConstantInt::get(Result, kTamperedResult);
  else if (Result->isFloatingPointTy())
    Wrong = llvm::ConstantFP::get(Result, kTamperedResult);
  if (Check == LeafCheck::ScratchResult || ScratchFrame ||
      Check == LeafCheck::NativeAddressing || Check == LeafCheck::NativeSIMD ||
      Check == LeafCheck::FramedError) {
    ASSERT_FALSE(Sum->isVarArg());
    ASSERT_TRUE(Sum->hasFnAttribute(llvm::Attribute::Naked));
  }
  if (Check == LeafCheck::FramedError)
    ASSERT_FALSE(Module.getFunction(kDifference)
                     ->hasFnAttribute(llvm::Attribute::Naked));
  if (Sum->hasFnAttribute(llvm::Attribute::Naked)) {
    auto *End = Sum->getEntryBlock().getTerminator();
    llvm::ReturnInst::Create(Context, Wrong, End->getIterator());
    End->eraseFromParent();
  } else {
    for (auto &Block : *Sum)
      if (auto *R = llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator()))
        R->setOperand(0, Wrong);
  }
  EXPECT_FALSE(Patcher.patch(Input, Rejected, *Changed, Image->Arch).Success);
  EXPECT_FALSE(std::filesystem::exists(Rejected));

  if (Check == LeafCheck::NativeSIMD) {
    auto Modified = llvm::CloneModule(Module);
    auto *F = Modified->getFunction(kSum);
    auto *Call = llvm::dyn_cast<llvm::CallInst>(&F->front().front());
    ASSERT_NE(Call, nullptr);
    auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
    ASSERT_NE(Asm, nullptr);
    std::string Text = Asm->getAsmString().str();
    const auto Begin = Text.find(kSIMDEncodedWord);
    ASSERT_NE(Begin, std::string::npos);
    const auto WordAt = Begin + std::string_view(kSIMDEncodedWord).size();
    const auto End = Text.find('\n', WordAt);
    ASSERT_NE(End, std::string::npos);
    uint32_t Word;
    ASSERT_FALSE(
        llvm::StringRef(Text).slice(WordAt, End).getAsInteger(10, Word));
    // Change the encoded destination without altering the function's ABI,
    // assembly shape, effects or relocations. Source binding must reject it.
    Text.replace(WordAt, End - WordAt,
                 std::to_string(Word ^ kSIMDDestinationBit));
    Call->setCalledOperand(llvm::InlineAsm::get(
        Asm->getFunctionType(), Text, Asm->getConstraintString(),
        Asm->hasSideEffects(), Asm->isAlignStack(), Asm->getDialect(),
        Asm->canThrow()));
    EXPECT_FALSE(
        Patcher.patch(Input, Rejected, *Modified, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }

  if (ScratchFrame) {
    // Withholding a generic body is only legal when its entire native body
    // can be installed. Even an unused, decodable instruction outside the
    // native producer's vocabulary must leave no partial module.
    const auto Unqualified = (Root / kUnknownStackInput).string();
    for (const auto *Unsupported :
         {kUnknownStackOperation, kUnknownVectorOperation}) {
      auto UnknownCompile = Compile;
      UnknownCompile.back() = Unqualified;
      UnknownCompile.insert(UnknownCompile.begin() + 1, Unsupported);
      ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, UnknownCompile), 0);
      auto UnknownImage = Reader->load(Unqualified);
      ASSERT_TRUE(bool(UnknownImage))
          << llvm::toString(UnknownImage.takeError());
      auto Refused = Pipeline().run(*UnknownImage, Context, Options);
      EXPECT_FALSE(Refused.Success);
      EXPECT_FALSE(Refused.LlvmModule);
    }

    auto ChangedBit = llvm::CloneModule(Module);
    auto *Entry = ChangedBit->getFunction(kDifference);
    ASSERT_NE(Entry, nullptr);
    ASSERT_TRUE(Entry->hasFnAttribute(llvm::Attribute::Naked));
    auto *Native = llvm::dyn_cast<llvm::CallInst>(&Entry->front().front());
    ASSERT_TRUE(Native && Native->isInlineAsm());
    const auto Bit = llvm::find_if(Native->args(), [](const llvm::Use &Arg) {
      const auto *C = llvm::dyn_cast<llvm::ConstantInt>(Arg.get());
      return C && C->equalsInt(kHighBranchBit);
    });
    ASSERT_NE(Bit, Native->arg_end());
    Bit->set(llvm::ConstantInt::get(Bit->get()->getType(), kHighBranchBit - 1));
    EXPECT_FALSE(
        Patcher.patch(Input, Rejected, *ChangedBit, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }
  if (Check == LeafCheck::ErrorResult || Check == LeafCheck::FramedError ||
      Indexed) {
    auto MissingWrite = llvm::CloneModule(Module);
    auto *Writer = MissingWrite->getFunction(kDifference);
    ASSERT_NE(Writer, nullptr);
    if (Writer->hasFnAttribute(llvm::Attribute::Naked)) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(&Writer->front().front());
      ASSERT_TRUE(Call && Call->isInlineAsm());
      auto *Assembly = llvm::cast<llvm::InlineAsm>(Call->getCalledOperand());
      std::string Text = Assembly->getAsmString().str();
      const auto At = Text.find(kNativeErrorWrite);
      ASSERT_NE(At, std::string::npos);
      Text.replace(At, llvm::StringRef(kNativeErrorWrite).size(),
                   kMissingReturnSign);
      Call->setCalledOperand(llvm::InlineAsm::get(
          Assembly->getFunctionType(), Text, Assembly->getConstraintString(),
          Assembly->hasSideEffects(), Assembly->isAlignStack(),
          Assembly->getDialect(), Assembly->canThrow()));
    } else {
      llvm::SmallVector<llvm::IntrinsicInst *> Writes;
      for (auto &BB : *Writer)
        for (auto &I : BB)
          if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
              Call && Call->getIntrinsicID() == llvm::Intrinsic::write_register)
            Writes.push_back(Call);
      ASSERT_EQ(Writes.size(), 1u);
      Writes.front()->eraseFromParent();
    }
    EXPECT_FALSE(
        Patcher.patch(Input, Rejected, *MissingWrite, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }
  if (Indexed || ScratchFrame || Check == LeafCheck::RestoringLoad) {
    auto Bytes = Snapshot;
    if (Indexed || ScratchFrame) {
      const auto *FrameSection =
          Image->getSectionByName(section_names::macho::EhFrame);
      ASSERT_NE(FrameSection, nullptr);
      auto Frame = llvm::MutableArrayRef(Bytes).slice(FrameSection->FileOff,
                                                      FrameSection->FileSz);
      llvm::ArrayRef<uint8_t> Rule = kLinkSpillRule;
      if (ScratchFrame)
        Rule = kFrameLinkSpillRule;
      const auto Find = [&](auto Begin) {
        return std::search(Begin, Frame.end(), Rule.begin(), Rule.end());
      };
      auto At = Find(Frame.begin());
      ASSERT_NE(At, Frame.end());
      ASSERT_EQ(Find(At + Rule.size()), Frame.end());
      At[Rule.size() - 1] =
          ScratchFrame ? kWrongFrameLinkOffsetFactor : kWrongLinkOffsetFactor;
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
    const auto Invalid = (Root / (Indexed || ScratchFrame ? kWrongSpillInput
                                                          : kPostIndexedInput))
                             .string();
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
    if (Entry->hasFnAttribute(llvm::Attribute::Naked)) {
      // Native bodies own their PAC instruction; removing a prologue
      // attribute alone does not remove their actual signing operation.
      auto *Call = llvm::cast<llvm::CallInst>(&Entry->front().front());
      auto *Assembly = llvm::cast<llvm::InlineAsm>(Call->getCalledOperand());
      std::string Text = Assembly->getAsmString().str();
      const auto At = Text.find(kNativeReturnSign);
      ASSERT_NE(At, std::string::npos);
      Text.replace(At, llvm::StringRef(kNativeReturnSign).size(),
                   kMissingReturnSign);
      Call->setCalledOperand(llvm::InlineAsm::get(
          Assembly->getFunctionType(), Text, Assembly->getConstraintString(),
          Assembly->hasSideEffects(), Assembly->isAlignStack(),
          Assembly->getDialect(), Assembly->canThrow()));
    } else {
      Entry->removeFnAttr(kReturnSigningScopeAttribute);
      Entry->removeFnAttr(kReturnSigningKeyAttribute);
    }
    EXPECT_FALSE(
        Patcher.patch(Input, Rejected, *Unsigned, Image->Arch).Success);
    EXPECT_FALSE(std::filesystem::exists(Rejected));
  }

  auto Installed = Patcher.patch(Input, Output, Module, Image->Arch);
  ASSERT_TRUE(Installed.Success);
  ASSERT_EQ(Installed.PatchedFunctions.size(), ExpectedEntries);
  auto Final = Reader->load(Output);
  ASSERT_TRUE(bool(Final)) << llvm::toString(Final.takeError());
  EXPECT_TRUE(Final->isArm64e());
  EXPECT_NE(Final->Raw, Snapshot);
  for (const auto &Entry : Installed.PatchedFunctions) {
    EXPECT_NE(Entry.OwnerVA, Entry.OriginalVA);
    EXPECT_TRUE(Final->ExceptionMetadata.findFunction(Entry.OwnerVA));
    if (ScratchFrame && Entry.SourceFunction == kDifference) {
      const auto *Before =
          Image->ExceptionMetadata.findFunction(Entry.OriginalVA);
      const auto *After = Final->ExceptionMetadata.findFunction(Entry.OwnerVA);
      ASSERT_TRUE(Before && Before->Dwarf && After && After->Dwarf);
      const auto *BeforeCIE =
          Image->ExceptionMetadata.findCIE(Before->Dwarf->CIESectionOffset);
      const auto *AfterCIE =
          Final->ExceptionMetadata.findCIE(After->Dwarf->CIESectionOffset);
      ASSERT_TRUE(BeforeCIE && AfterCIE);
      EXPECT_TRUE(BeforeCIE->HasPointerAuth);
      EXPECT_TRUE(AfterCIE->HasPointerAuth);
      EXPECT_EQ(BeforeCIE->Augmentation, AfterCIE->Augmentation);
      const auto Effective = [](const auto &I) {
        return I.Kind != CFIOpKind::Nop;
      };
      EXPECT_TRUE(llvm::equal(
          llvm::make_filter_range(Before->Dwarf->Instructions, Effective),
          llvm::make_filter_range(After->Dwarf->Instructions, Effective),
          dwarf_eh::sameCFIInstruction));
      for (auto Kind : {CFIOpKind::DefCFA, CFIOpKind::DefCFARegister,
                        CFIOpKind::RememberState, CFIOpKind::RestoreState,
                        CFIOpKind::SameValue, CFIOpKind::GnuArgsSize})
        EXPECT_TRUE(
            llvm::any_of(After->Dwarf->Instructions,
                         [&](const auto &I) { return I.Kind == Kind; }));
    }
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
  ASSERT_GE(AuthenticatedCalls, Check == LeafCheck::NativeSIMD
                                    ? kSharedDispatchAuthenticatedSites
                                    : kCount);
  testing::Test::RecordProperty(kAuthenticatedSites, AuthenticatedCalls);
  std::vector<std::string> ExecutionPaths{Input, Output};
  if (Check == LeafCheck::Resigned || Check == LeafCheck::Relifted) {
    std::unique_ptr<llvm::MemoryBuffer> NativeObject;
    if (Check == LeafCheck::Resigned) {
      const auto Object = (Root / kRepublishObject).string();
      ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, {kCompiler, kTargetFlag,
                                                      kTarget, kCompileObject,
                                                      Source, kOut, Object}),
                0);
      auto File = llvm::MemoryBuffer::getFile(Object);
      ASSERT_TRUE(bool(File));
      NativeObject = std::move(*File);
    }
    for (unsigned I = 0; I < kRepeatedPublications; ++I) {
      SCOPED_TRACE(I);
      const auto PreviousPath = ExecutionPaths.back();
      auto Previous = Reader->load(PreviousPath);
      ASSERT_TRUE(bool(Previous)) << llvm::toString(Previous.takeError());
      auto Before = macho_signature::inspect(Previous->Raw);
      ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
      ASSERT_TRUE(macho_signature::isCanonicalCodesignAdHocOutput(*Before));
      const auto NextPath =
          (Root / (kRepeatedPrefix + std::to_string(I) + kOutput)).string();
      PatchResult Repeated;
      if (Check == LeafCheck::Relifted) {
        testing::Test::RecordProperty(kReliftStartsPrefix + std::to_string(I),
                                      Previous->MachOHasFunctionStarts ? 1 : 0);
        const auto *Unwind =
            Previous->getSectionByName(section_names::macho::Unwind);
        ASSERT_NE(Unwind, nullptr);
        EXPECT_FALSE(Previous->hasAuthenticatedFunctionEntryAt(Unwind->VA));
        llvm::LLVMContext NextContext;
        auto Next = Pipeline().run(*Previous, NextContext, Options);
        for (const auto &Audit : Next.FunctionAudits)
          testing::Test::RecordProperty(
              kReliftAuditPrefix + std::to_string(I) + kReliftSeparator +
                  llvm::utohexstr(Audit.Entry),
              pipelineFunctionDispositionName(Audit.Disposition));
        ASSERT_TRUE(Next.Success) << Next.Error;
        ASSERT_TRUE(Next.LlvmModule);
        std::vector<va_t> Entries;
        for (const auto &Audit : Next.FunctionAudits) {
          if (Audit.HasLLVMDefinition)
            Entries.push_back(Audit.Entry);
          else
            EXPECT_EQ(Audit.Disposition,
                      PipelineFunctionDisposition::SkippedImportStub);
        }
        ASSERT_EQ(Entries.size(), ExpectedEntries << (I + 1));
        testing::Test::RecordProperty(
            kReliftFunctionsPrefix + std::to_string(I), int(Entries.size()));
        unsigned CheckedForwarders = 0;
        for (size_t J = 0; J < Entries.size(); ++J) {
          SCOPED_TRACE(llvm::utohexstr(Entries[J]));
          const auto Body = machOLiftedSource(
              *Previous, Entries[J],
              J + 1 == Entries.size() ? InvalidVA : Entries[J + 1]);
          ASSERT_TRUE(Body);
          if (Body->ForwardTarget && !Previous->getSectionFor(Entries[J])) {
            ++CheckedForwarders;
            auto Overlap = *Previous;
            Section Data;
            Data.VA = Entries[J] + Body->Bytes.size();
            Data.Size = sizeof(uint8_t);
            Overlap.Sections.push_back(Data);
            EXPECT_TRUE(machOLiftedSource(Overlap, Entries[J], InvalidVA));
            --Overlap.Sections.back().VA;
            EXPECT_FALSE(machOLiftedSource(Overlap, Entries[J], InvalidVA));
          }
        }
        if (I)
          EXPECT_GT(CheckedForwarders, 0u);
        // A compact extent must not turn intervening data or virtual-only
        // bytes into the tail of a sectionless installed function.
        const auto InstalledBody = llvm::find_if(Entries, [&](va_t Entry) {
          if (Previous->getSectionFor(Entry))
            return false;
          const auto Body = machOLiftedSource(*Previous, Entry, InvalidVA);
          return Body && !Body->ForwardTarget &&
                 Body->Bytes.size() > aarch64::kInsnSize;
        });
        ASSERT_NE(InstalledBody, Entries.end());
        const auto Last = *InstalledBody;
        auto Overlap = *Previous;
        Section Data;
        Data.VA = Last + aarch64::kInsnSize;
        Data.Size = aarch64::kInsnSize;
        Overlap.Sections.push_back(Data);
        EXPECT_FALSE(machOLiftedSource(Overlap, Last, InvalidVA));
        auto Truncated = *Previous;
        auto Segment = llvm::find_if(Truncated.Segments, [&](const auto &S) {
          return S.contains(Last);
        });
        ASSERT_NE(Segment, Truncated.Segments.end());
        Segment->FileSz = Last - Segment->VA + aarch64::kInsnSize - 1;
        EXPECT_FALSE(machOLiftedSource(Truncated, Last, InvalidVA));
        symbolizeImageAbsolutePointers(*Next.LlvmModule, *Previous);
        MachOPatcher NextPatcher;
        NextPatcher.setImageContext(&*Previous);
        auto Redirected = llvm::CloneModule(*Next.LlvmModule);
        auto *Forwarder = Redirected->getFunction(kSum);
        ASSERT_TRUE(Forwarder &&
                    Forwarder->hasFnAttribute(llvm::Attribute::Naked));
        auto *Branch =
            llvm::dyn_cast<llvm::CallInst>(&Forwarder->front().front());
        ASSERT_TRUE(Branch && Branch->isInlineAsm());
        ASSERT_EQ(Branch->arg_size(), 1u);
        Branch->setArgOperand(0, Forwarder);
        EXPECT_FALSE(
            NextPatcher
                .patch(PreviousPath, Rejected, *Redirected, Previous->Arch)
                .Success);
        EXPECT_FALSE(std::filesystem::exists(Rejected));
        auto Partial = llvm::CloneModule(*Next.LlvmModule);
        Partial->getFunction(kDifference)->deleteBody();
        EXPECT_FALSE(
            NextPatcher.patch(PreviousPath, Rejected, *Partial, Previous->Arch)
                .Success);
        EXPECT_FALSE(std::filesystem::exists(Rejected));
        Repeated = NextPatcher.patch(PreviousPath, NextPath, *Next.LlvmModule,
                                     Previous->Arch);
      } else {
        std::vector<SourcePatchMapping> Mappings;
        for (const auto &[Name, Symbol] :
             {std::pair{kSum, kSumSymbol},
              std::pair{kDifference, kDifferenceSymbol}}) {
          const auto *Entry = Previous->findSymbol(Symbol);
          ASSERT_NE(Entry, nullptr);
          Mappings.push_back({Name, Entry->Addr});
        }
        auto Native = installMachOObjectPatch(
            NativeObject->getBuffer(), *Previous, Mappings, {}, NextPath, {});
        ASSERT_TRUE(bool(Native)) << llvm::toString(Native.takeError());
        Repeated = std::move(*Native);
        ASSERT_EQ(Repeated.PatchedFunctions.size(), ExpectedEntries);
      }
      ASSERT_TRUE(Repeated.Success);
      ASSERT_FALSE(Repeated.PatchedFunctions.empty());
      for (const auto &Entry : Repeated.PatchedFunctions)
        EXPECT_NE(Entry.OwnerVA, Entry.OriginalVA);
      auto Reloaded = Reader->load(NextPath);
      ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
      auto After = macho_signature::inspect(Reloaded->Raw);
      ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
      auto Validation = macho_signature::validateTransactionallyAdHocResigned(
          *Before, *After);
      ASSERT_FALSE(bool(Validation)) << llvm::toString(std::move(Validation));
      auto Untouched = Reader->load(PreviousPath);
      ASSERT_TRUE(bool(Untouched)) << llvm::toString(Untouched.takeError());
      EXPECT_EQ(Untouched->Raw, Previous->Raw);
      EXPECT_NE(Reloaded->Raw, Previous->Raw);
      testing::Test::RecordProperty(
          kRepeatedHashPrefix + std::to_string(I),
          llvm::toHex(llvm::SHA256::hash(Reloaded->Raw), true));
      ExecutionPaths.push_back(NextPath);
    }
    if (Embedded) {
      for (const auto &[Generation, Path] : llvm::enumerate(ExecutionPaths)) {
        auto Loaded = Reader->load(Path);
        ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
        const auto &Records = Loaded->MachODataInCode;
        ASSERT_EQ(Records.size(),
                  Image->MachODataInCode.size() * (Generation + 1));
        for (const auto &[I, R] : llvm::enumerate(Records)) {
          const auto &Source =
              Image->MachODataInCode[I % Image->MachODataInCode.size()];
          ASSERT_EQ(R.End - R.Begin, Source.End - Source.Begin);
          EXPECT_EQ(R.Kind, Source.Kind);
          const auto *Old =
              Image->readVA(Source.Begin, Source.End - Source.Begin);
          const auto *New = Loaded->readVA(R.Begin, R.End - R.Begin);
          ASSERT_TRUE(Old && New);
          EXPECT_EQ(llvm::ArrayRef(Old, Source.End - Source.Begin),
                    llvm::ArrayRef(New, R.End - R.Begin));
          EXPECT_FALSE(Loaded->hasExecutableCodeOwnerAt(R.Begin));
        }
        if (Generation + 1 == ExecutionPaths.size())
          testing::Test::RecordProperty(kMixedRecords, int(Records.size()));
      }
    }
    for (const auto &Path : ExecutionPaths) {
      if (Preamble == kSplitData || Preamble == kSplitSegments) {
        auto Loaded = Reader->load(Path);
        ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
        const auto *Marker = Loaded->findSymbol(kMarkerSymbol);
        ASSERT_TRUE(Marker);
        const auto *Data =
            Loaded->readVA(Marker->Addr, sizeof(kMarkerBytes) - 1);
        ASSERT_TRUE(Data);
        EXPECT_EQ(llvm::StringRef(reinterpret_cast<const char *>(Data),
                                  sizeof(kMarkerBytes) - 1),
                  kMarkerBytes);
      }
      auto Verified = macho_signing::verifyStrict(Path);
      ASSERT_FALSE(bool(Verified)) << llvm::toString(std::move(Verified));
    }
    testing::Test::RecordProperty(kPublicationGenerations,
                                  1 + kRepeatedPublications);
  }
  if (std::getenv(kExecute)) {
    for (const auto &Path : ExecutionPaths) {
      SCOPED_TRACE(Path);
      ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, Path}), 0);
    }
    if (Check == LeafCheck::TailTarget) {
      ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, ReorderedPath}), 0);
      testing::Test::RecordProperty(kReorderedTail, true);
    }
    if (Preamble == kDispatchOracle)
      testing::Test::RecordProperty(kDispatchComparisons,
                                    ExecutionPaths.size() * kDispatchFunctions *
                                        kDispatchInputs * kDispatchArguments);
    testing::Test::RecordProperty(kNativeImages, ExecutionPaths.size());
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

void verifyVariadicRegisterImage(const char *Optimization) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kTemporary, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Input = (Root / kOriginal).string();
  const auto Output = (Root / kOutput).string();
  const auto Rejected = (Root / kRejected).string();
  const auto Source = (Root / kSource).string();
  std::ofstream(Source) << kVariadicLeaves;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                kCompiler, {kCompiler, kTargetFlag, kTarget, Optimization,
                            kOmitFrame, kDylib, Source, kOut, Input}),
            0);
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Image = Reader->load(Input);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  PipelineOptions Options;
  Options.PatchMode = true;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  auto Lift = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Lift.Success) << Lift.Error;
  ASSERT_TRUE(Lift.LlvmModule);
  ASSERT_EQ(Lift.FunctionAudits.size(), kMixedEntries);
  for (const auto &Audit : Lift.FunctionAudits)
    ASSERT_TRUE(Audit.HasLLVMDefinition) << Audit.Name;
  auto &Module = *Lift.LlvmModule;
  symbolizeImageAbsolutePointers(Module, *Image);
  MachOPatcher Patcher;
  Patcher.setImageContext(&*Image);

  auto Wrong = llvm::CloneModule(Module);
  auto *Difference = Wrong->getFunction(kDifference);
  ASSERT_TRUE(Difference && !Difference->empty());
  auto *Call = llvm::dyn_cast<llvm::CallInst>(&Difference->front().front());
  ASSERT_TRUE(Call && Call->isInlineAsm());
  auto *Asm = llvm::cast<llvm::InlineAsm>(Call->getCalledOperand());
  std::string ChangedAsm = Asm->getAsmString().str();
  const auto Return = ChangedAsm.rfind(kReturnMnemonic);
  ASSERT_NE(Return, std::string::npos);
  ChangedAsm.insert(Return, kChangeScratch);
  Call->setCalledOperand(llvm::InlineAsm::get(
      Call->getFunctionType(), ChangedAsm, Asm->getConstraintString(), true));
  EXPECT_FALSE(Patcher.patch(Input, Rejected, *Wrong, Image->Arch).Success);
  EXPECT_FALSE(std::filesystem::exists(Rejected));

  Wrong = llvm::CloneModule(Module);
  auto *ImmediateCall = llvm::cast<llvm::CallInst>(
      &Wrong->getFunction(kDifference)->front().front());
  auto Immediate =
      llvm::find_if(ImmediateCall->args(), [](const llvm::Use &Arg) {
        return llvm::isa<llvm::ConstantInt>(Arg.get());
      });
  ASSERT_NE(Immediate, ImmediateCall->arg_end());
  auto *Amount = llvm::cast<llvm::ConstantInt>(Immediate->get());
  Immediate->set(
      llvm::ConstantInt::get(Amount->getType(), Amount->getValue() + 1));
  EXPECT_FALSE(Patcher.patch(Input, Rejected, *Wrong, Image->Arch).Success);
  EXPECT_FALSE(std::filesystem::exists(Rejected));

  Wrong = llvm::CloneModule(Module);
  auto *Direct = Wrong->getFunction(kVariadicDirect);
  ASSERT_TRUE(Direct && !Direct->empty());
  auto *Branch = llvm::dyn_cast<llvm::CallInst>(&Direct->front().front());
  ASSERT_TRUE(Branch && Branch->isInlineAsm());
  auto Target = llvm::find_if(Branch->args(), [](const llvm::Use &Arg) {
    return llvm::isa<llvm::Function>(Arg.get());
  });
  ASSERT_NE(Target, Branch->arg_end());
  Target->set(Wrong->getFunction(kDifference));
  EXPECT_FALSE(Patcher.patch(Input, Rejected, *Wrong, Image->Arch).Success);
  EXPECT_FALSE(std::filesystem::exists(Rejected));

  auto Installed = Patcher.patch(Input, Output, Module, Image->Arch);
  ASSERT_TRUE(Installed.Success);
  ASSERT_EQ(Installed.PatchedFunctions.size(), kMixedEntries);
  testing::Test::RecordProperty(kInstalledEntries, kMixedEntries);
  auto Final = Reader->load(Output);
  ASSERT_TRUE(bool(Final)) << llvm::toString(Final.takeError());
  for (const auto &Entry : Installed.PatchedFunctions) {
    EXPECT_NE(Entry.OriginalVA, Entry.OwnerVA);
    EXPECT_TRUE(Final->ExceptionMetadata.findFunction(Entry.OwnerVA));
  }
  EXPECT_EQ(llvm::sys::ExecuteAndWait(kCodesign,
                                      {kCodesign, kVerify, kStrict, Output}),
            0);
  testing::Test::RecordProperty(
      kOriginalHash, llvm::toHex(llvm::SHA256::hash(Image->Raw), true));
  testing::Test::RecordProperty(
      kPublishedHash, llvm::toHex(llvm::SHA256::hash(Final->Raw), true));
  if (!std::getenv(kExecute))
    return;
  const auto Caller = (Root / kDriverSource).string();
  const auto Driver = (Root / kDriver).string();
  std::ofstream(Caller) << kExtendedVariadics << kVariadicCaller;
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(kCompiler, {kCompiler, kTargetFlag, kTarget,
                                            kOptimize, Caller, kOut, Driver}),
      0);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, Input}), 0);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(Driver, {Driver, Output}), 0);
  const auto RegisterC = (Root / kRegisterCallerC).string();
  const auto RegisterS = (Root / kRegisterCallerName).string();
  const auto Observer = (Root / kRegisterDriver).string();
  std::ofstream(RegisterC) << kRegisterCallerSource;
  std::ofstream(RegisterS) << kRegisterCallerAssembly;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, {kCompiler, kTargetFlag,
                                                  kTarget, kOptimize, RegisterC,
                                                  RegisterS, kOut, Observer}),
            0);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(Observer, {Observer, Input, Output}), 0);
  testing::Test::RecordProperty(kNativeImages, kCount);
  testing::Test::RecordProperty(kRegisterObservations, kMixedEntries);
  testing::Test::RecordProperty(kRegisterMatrix,
                                kMixedEntries * kRegisterInputs);
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64DarwinEntry, DirectCallerConsumesNonVariadicX8AndX9Results) {
  verifyPublishedLeaves(kScratchResultSource, kCaller, kAssemblyName, {},
                        LeafCheck::ScratchResult);
}

TEST(AArch64DarwinEntry, DirectCallerConsumesTailForwardedX8AndX9Results) {
  verifyPublishedLeaves(kScratchResultSource, kCaller, kAssemblyName,
                        kScratchTail, LeafCheck::ScratchResult,
                        kScratchTailEntries);
}

TEST(AArch64DarwinEntry, DirectCallResultsSurviveForwardAndBackwardBranches) {
  verifyPublishedLeaves(kScratchResultSource, kCaller, kAssemblyName,
                        kScratchBranches, LeafCheck::ScratchResult);
}

TEST(AArch64DarwinEntry, BranchWritesKillBothScratchResultsBeforeUse) {
  verifyPublishedLeaves(kScratchResultSource, kCaller, kAssemblyName,
                        kScratchKilled, LeafCheck::KilledScratchResult);
}

TEST(AArch64DarwinEntry, MultipleEpiloguesKeepFramePointerAndRememberedRows) {
  verifyPublishedLeaves(kScratchResultSource, kCaller, kAssemblyName,
                        kScratchFrame, LeafCheck::FrameScratchResult);
}
TEST(AArch64DarwinEntry, SIMDDataPreservesIndexedStackAndRememberedRows) {
  verifyPublishedLeaves(kScratchResultSource, kCaller, kAssemblyName,
                        kScratchFrameSIMD, LeafCheck::FrameScratchResult);
}

TEST(AArch64DarwinVariadic, OptimizedImagePreservesReturnedRegisters) {
  verifyVariadicRegisterImage(kOptimize);
}

TEST(AArch64DarwinVariadic, UnoptimizedImagePreservesReturnedRegisters) {
  verifyVariadicRegisterImage(kUnoptimized);
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

TEST(AArch64LiftedImage,
     CodesignSignedLiftedOutputAllowsRepeatedNativePublication) {
  verifyPublishedLeaves(kSignedLeaves, kCaller, kAssemblyName, {},
                        LeafCheck::Resigned);
}

TEST(AArch64LiftedImage,
     EmbeddedLiteralAndJumpTablesSurviveAutomaticRelifting) {
  verifyPublishedLeaves(kMixedLeaves, kCaller, kAssemblyName, kMixedOracle,
                        LeafCheck::Relifted);
}

TEST(AArch64LiftedImage, RelativeSwitchDispatchSurvivesAutomaticRelifting) {
  verifyPublishedLeaves(kDispatchLeaves, kDispatchCaller, kAssemblyName,
                        kDispatchOracle, LeafCheck::Relifted,
                        kDispatchFunctions);
}

TEST(AArch64LiftedImage, SeparateInstructionSectionsSurviveAutomaticRelifting) {
  verifyPublishedLeaves(kSplitLeaves, kCaller, kSource, kSplitData,
                        LeafCheck::Relifted);
}

TEST(AArch64LiftedImage, SeparateExecutableSegmentsSurviveAutomaticRelifting) {
  verifyPublishedLeaves(kSplitLeaves, kCaller, kSource, kSplitSegments,
                        LeafCheck::Relifted);
}

TEST(AArch64LiftedImage, AppendedFunctionsSurviveRepeatedAutomaticLifting) {
  verifyPublishedLeaves(kSignedLeaves, kCaller, kAssemblyName, {},
                        LeafCheck::Relifted);
}

TEST(AArch64LiftedImage, AppendedMemorySurvivesRepeatedAutomaticLifting) {
  verifyPublishedLeaves(kVectorCopyLeaves, kVectorCopyCaller, kAssemblyName, {},
                        LeafCheck::Relifted);
}

TEST(AArch64LiftedImage, AppendedSIMDRegistersSurviveRepeatedAutomaticLifting) {
  verifyPublishedLeaves(kSIMDRegisterLeaves, kSIMDRegisterCaller, kAssemblyName,
                        {}, LeafCheck::Relifted);
}

TEST(AArch64LiftedImage, AppendedIndexedFramesSurviveRepeatedAutomaticLifting) {
  verifyPublishedLeaves(kScratchResultSource, kCaller, kAssemblyName,
                        kScratchFrameSIMD, LeafCheck::Relifted);
}

TEST(AArch64LiftedImage,
     AppendedGlobalAndImportSurviveRepeatedAutomaticLifting) {
  verifyPublishedLeaves(kGlobalLeaves, kGlobalCaller, kSource, {},
                        LeafCheck::Relifted);
}

TEST(AArch64LiftedImage, AdHocRequirementsRefusePublicationWithoutMutation) {
  verifyPublishedLeaves(kSignedLeaves, kCaller, kAssemblyName, {},
                        LeafCheck::SigningRequirements);
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
TEST(AArch64LiftedImage, PublishesSignedAndUnsignedSelections) {
  verifyPublishedLeaves(kScalarLeaves, kScalarCaller);
}
TEST(AArch64LiftedImage, PublishesArchitecturalDivisionAtExceptionalInputs) {
  verifyPublishedLeaves(kDivisionLeaves, kScalarCaller, kAssemblyName,
                        kDivisionOracle);
}
TEST(AArch64LiftedImage, PublishesWordDivisionAtExceptionalInputs) {
  verifyPublishedLeaves(kDivisionLeaves, kScalarCaller, kAssemblyName,
                        kWordDivisionOracle);
}
TEST(AArch64LiftedImage, PublishesConditionalReadsAndWrites) {
  verifyPublishedLeaves(kConditionalLeaves, kConditionalCaller);
}
TEST(AArch64LiftedImage, PublishesLoopsWithPostIndexedMemory) {
  verifyPublishedLeaves(kLoopLeaves, kLoopCaller, kSource, {},
                        LeafCheck::NativeAddressing);
}
TEST(AArch64LiftedImage, PublishesScaledAndSignedRegisterOffsets) {
  verifyPublishedLeaves(kIndexedLeaves, kIndexedCaller, kSource, {},
                        LeafCheck::NativeAddressing);
}
TEST(AArch64LiftedImage, PublishesSIMD128SingleAndPairWritebackLoops) {
  verifyPublishedLeaves(kVectorCopyLeaves, kVectorCopyCaller, kAssemblyName, {},
                        LeafCheck::NativeAddressing);
}
TEST(AArch64LiftedImage, PublishesSIMDByteWritebackLoops) {
  verifyPublishedLeaves(kVectorCopyLeaves, kVectorCopyCaller, kAssemblyName,
                        kVectorByteMemory, LeafCheck::NativeAddressing);
}
TEST(AArch64LiftedImage, PublishesSIMDHalfWritebackLoops) {
  verifyPublishedLeaves(kVectorCopyLeaves, kVectorCopyCaller, kAssemblyName,
                        kVectorHalfMemory, LeafCheck::NativeAddressing);
}
TEST(AArch64LiftedImage, PublishesSIMDWordAndPairWritebackLoops) {
  verifyPublishedLeaves(kVectorCopyLeaves, kVectorCopyCaller, kAssemblyName,
                        kVectorWordMemory, LeafCheck::NativeAddressing);
}
TEST(AArch64LiftedImage, PublishesSIMDDoubleAndPairWritebackLoops) {
  verifyPublishedLeaves(kVectorCopyLeaves, kVectorCopyCaller, kAssemblyName,
                        kVectorDoubleMemory, LeafCheck::NativeAddressing);
}
TEST(AArch64LiftedImage, PublishesSIMD16ByteArithmetic) {
  verifyPublishedLeaves(kSIMDArithmeticLeaves, kSIMDArithmeticCaller,
                        kAssemblyName, {}, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMD8ByteArithmetic) {
  verifyPublishedLeaves(kSIMDArithmeticLeaves, kSIMDArithmeticCaller,
                        kAssemblyName, kSIMD8B, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMD8HalfArithmetic) {
  verifyPublishedLeaves(kSIMDArithmeticLeaves, kSIMDArithmeticCaller,
                        kAssemblyName, kSIMD8H, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMD4HalfArithmetic) {
  verifyPublishedLeaves(kSIMDArithmeticLeaves, kSIMDArithmeticCaller,
                        kAssemblyName, kSIMD4H, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMD4WordArithmetic) {
  verifyPublishedLeaves(kSIMDArithmeticLeaves, kSIMDArithmeticCaller,
                        kAssemblyName, kSIMD4S, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMD2WordArithmetic) {
  verifyPublishedLeaves(kSIMDArithmeticLeaves, kSIMDArithmeticCaller,
                        kAssemblyName, kSIMD2S, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMD2DoubleArithmetic) {
  verifyPublishedLeaves(kSIMDArithmeticLeaves, kSIMDArithmeticCaller,
                        kAssemblyName, kSIMD2D, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMDScalarDoubleArithmetic) {
  verifyPublishedLeaves(kSIMDArithmeticLeaves, kSIMDArithmeticCaller,
                        kAssemblyName, kSIMD1D, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMDPermutationsTablesAndLanes) {
  verifyPublishedLeaves(kSIMDPermutationLeaves, kSIMDPermutationCaller,
                        kAssemblyName, {}, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMDFusedLanesWithFloatingEnvironment) {
  verifyPublishedLeaves(kSIMDFloatingLeaves, kSIMDFloatingCaller, kAssemblyName,
                        {}, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMDRegisterArgumentsAndResults) {
  verifyPublishedLeaves(kSIMDRegisterLeaves, kSIMDRegisterCaller, kAssemblyName,
                        {}, LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesSIMDWidenNarrowReduceAndSaturationState) {
  verifyPublishedLeaves(kSIMDWidenLeaves, kSIMDWidenCaller, kAssemblyName, {},
                        LeafCheck::NativeSIMD);
}
TEST(AArch64LiftedImage, PublishesPostIndexedScalarAndPairMemory) {
  verifyPublishedLeaves(kScalarWritebackLeaves, kScalarWritebackCaller,
                        kAssemblyName, {}, LeafCheck::NativeAddressing);
}
TEST(AArch64LiftedImage, PublishesPreIndexedScalarAndPairMemory) {
  verifyPublishedLeaves(kScalarWritebackLeaves, kScalarWritebackCaller,
                        kAssemblyName, kPreIndexedMemory,
                        LeafCheck::NativeAddressing);
}
TEST(AArch64LiftedImage, PublishesSignAndZeroExtendingLoads) {
  verifyPublishedLeaves(kNarrowLeaves, kNarrowCaller);
}
TEST(AArch64LiftedImage, PublishesByteReversalAndTrailingZeroCount) {
  verifyPublishedLeaves(kBitLeaves, kBitCaller);
}
TEST(AArch64LiftedImage, PublishesMultiplyAccumulateAndVariableShift) {
  verifyPublishedLeaves(kMultiplyLeaves, kMultiplyCaller);
}

TEST(AArch64LiftedImage, PublishesNewThreadLocalDefinitionsAndInitializers) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kTemporary, Dir));
  const std::filesystem::path Root(Dir.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kNewTLSFile).string(),
             Base = (Root / kNewTLSImage).string();
  const auto Input = (Root / kOriginal).string(),
             Output = (Root / kOutput).string();
  const auto Reference = (Root / kTLVProvider).string(),
             Object = (Root / kTLVObject).string();
  const auto DriverSource = (Root / kNewTLSDriverFile).string(),
             Driver = (Root / kDriver).string();
  std::ofstream(Source) << kNewTLSBody;
  std::ofstream(Base) << kNewTLSBase;
  std::ofstream(DriverSource) << kNewTLSDriver;
  auto Reader = Loader::create(BinaryFormat::MachO);
  unsigned Runs = 0;
  for (const auto *Opt : {kUnoptimized, kOptimize}) {
    for (bool OldTLS : {false, true}) {
      SCOPED_TRACE(Opt);
      SCOPED_TRACE(OldTLS);
      const auto Compile = [&](llvm::ArrayRef<llvm::StringRef> Args) {
        llvm::SmallVector<llvm::StringRef> Cmd{kTLSCxx, kTargetFlag, kTarget,
                                               Opt};
        if (OldTLS)
          Cmd.push_back(kTLSWithOriginal);
        Cmd.append(Args.begin(), Args.end());
        return llvm::sys::ExecuteAndWait(kTLSCxx, Cmd);
      };
      ASSERT_EQ(Compile({kDylib, Base, kOut, Input}), 0);
      ASSERT_EQ(Compile({kDylib, kTLSReference, Base, Source, kOut, Reference}),
                0);
      ASSERT_EQ(Compile({kTLVCompile, Source, kOut, Object}), 0);
      ASSERT_EQ(Compile({DriverSource, kOut, Driver}), 0);
      auto Original = Reader->load(Input);
      ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
      const auto *Entry = Original->findSymbol(kTLVSymbol);
      ASSERT_NE(Entry, nullptr);
      const SourcePatchMapping Map{kTLVMapping, Entry->Addr};
      auto Bytes = llvm::MemoryBuffer::getFile(Object);
      ASSERT_TRUE(bool(Bytes));
      auto Appended = Original->Raw;
      auto Native = appendMachOObjectImage(Appended, (*Bytes)->getBuffer(), {});
      ASSERT_TRUE(bool(Native)) << llvm::toString(Native.takeError());
      auto AppendedImage = Reader->loadBuffer(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Appended.data()),
                          Appended.size()),
          {}));
      ASSERT_TRUE(bool(AppendedImage))
          << llvm::toString(AppendedImage.takeError());
      EXPECT_TRUE(llvm::any_of(AppendedImage->Exports, [](const auto &E) {
        return E.Name == kPublishedTLVName;
      }));
      auto Patch = installMachOObjectPatch((*Bytes)->getBuffer(), *Original,
                                           {Map}, {}, Output, {});
      ASSERT_TRUE(bool(Patch)) << llvm::toString(Patch.takeError());
      auto Installed = Reader->load(Output);
      ASSERT_TRUE(bool(Installed)) << llvm::toString(Installed.takeError());
      const Section *Template = nullptr;
      for (const auto &S : Installed->Sections)
        if ((S.Type & llvm::MachO::SECTION_TYPE) ==
            llvm::MachO::S_THREAD_LOCAL_REGULAR) {
          ASSERT_FALSE(Template);
          Template = &S;
        }
      ASSERT_NE(Template, nullptr);
      auto Export = llvm::find_if(Installed->Exports, [](const auto &E) {
        return E.Name == kPublishedTLVName;
      });
      ASSERT_NE(Export, Installed->Exports.end());
      const auto *ExportSection = Installed->getSectionFor(Export->Addr);
      ASSERT_TRUE(ExportSection);
      EXPECT_EQ(ExportSection->Type & llvm::MachO::SECTION_TYPE,
                llvm::MachO::S_THREAD_LOCAL_VARIABLES);
      unsigned Descriptors = 0;
      for (const auto &S : Installed->Sections) {
        if ((S.Type & llvm::MachO::SECTION_TYPE) !=
            llvm::MachO::S_THREAD_LOCAL_VARIABLES)
          continue;
        for (uint64_t Off = 0; Off < S.Size;
             Off += kTLVDescriptorWords * sizeof(uint64_t)) {
          auto Recipe = Installed->MachOAuthenticatedPointers.find(S.VA + Off);
          ASSERT_NE(Recipe, Installed->MachOAuthenticatedPointers.end());
          EXPECT_EQ(Recipe->second.Symbol, kTLVResolverSymbol);
          EXPECT_FALSE(Recipe->second.AddressDiversity);
          EXPECT_EQ(Recipe->second.Key, 0u);
          EXPECT_EQ(Recipe->second.Diversity, 0u);
          const auto *Offset = Installed->readVA(
              S.VA + Off + kTLVOffsetWord * sizeof(uint64_t), sizeof(uint64_t));
          ASSERT_NE(Offset, nullptr);
          EXPECT_LT(readLE<uint64_t>(Offset), Template->Size);
          ++Descriptors;
        }
      }
      EXPECT_GE(Descriptors, kTLVNewDescriptors + unsigned(OldTLS));
      if (std::getenv(kExecute))
        for (const auto &Image : {Reference, Output}) {
          ASSERT_EQ(llvm::sys::ExecuteAndWait(
                        Driver, {Driver, Image, std::to_string(OldTLS)}),
                    0);
          ++Runs;
        }
      ASSERT_TRUE(std::filesystem::remove(Output));
      // A new template must not hide ambiguity in the original export scope.
      ASSERT_EQ(Compile({kDylib, kTLSConflict, Base, kOut, Input}), 0);
      auto Conflict = Reader->load(Input);
      ASSERT_TRUE(bool(Conflict)) << llvm::toString(Conflict.takeError());
      const auto *ConflictEntry = Conflict->findSymbol(kTLVSymbol);
      ASSERT_NE(ConflictEntry, nullptr);
      SourcePatchMapping Collision{kTLVMapping, ConflictEntry->Addr};
      auto Failed = installMachOObjectPatch((*Bytes)->getBuffer(), *Conflict,
                                            {Collision}, {}, Output, {});
      ASSERT_FALSE(bool(Failed));
      llvm::consumeError(Failed.takeError());
      EXPECT_FALSE(std::filesystem::exists(Output));
      // dyld owns the initially zero key word. Reject a producer trying to
      // preseed it and verify publication leaves no file behind.
      std::string Changed = (*Bytes)->getBuffer().str();
      auto File = llvm::object::ObjectFile::createObjectFile(
          llvm::MemoryBufferRef(Changed, {}));
      ASSERT_TRUE(bool(File)) << llvm::toString(File.takeError());
      bool Mutated = false;
      for (const auto &S : (*File)->sections()) {
        auto Name = S.getName();
        ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
        if (*Name != kTLSDescriptorSection)
          continue;
        auto Contents = S.getContents();
        ASSERT_TRUE(bool(Contents)) << llvm::toString(Contents.takeError());
        ASSERT_GE(Contents->size(), kTLVDescriptorWords * sizeof(uint64_t));
        Changed[Contents->data() - Changed.data() + sizeof(uint64_t)] = 1;
        Mutated = true;
      }
      ASSERT_TRUE(Mutated);
      Failed =
          installMachOObjectPatch(Changed, *Original, {Map}, {}, Output, {});
      ASSERT_FALSE(bool(Failed));
      llvm::consumeError(Failed.takeError());
      EXPECT_FALSE(std::filesystem::exists(Output));
    }
  }
  RecordProperty(kNewTLSProperty, Runs);
#else
  GTEST_SKIP() << kUnavailable;
#endif
}
TEST(AArch64LiftedImage, RunsLegacyThreadInitializersWithPerInstancePAC) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kTemporary, Dir));
  const std::filesystem::path Root(Dir.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kNewTLSFile).string(),
             Base = (Root / kNewTLSImage).string(),
             DriverSource = (Root / kNewTLSDriverFile).string(),
             Driver = (Root / kDriver).string(),
             Reference = (Root / kTLVProvider).string(),
             Input = (Root / kOriginal).string(),
             Output = (Root / kOutput).string(),
             Object = (Root / kTLVObject).string();
  std::ofstream(Source) << kPerThreadBody;
  std::ofstream(Base) << kPerThreadBase;
  std::ofstream(DriverSource) << kPerThreadDriver;
  auto Reader = Loader::create(BinaryFormat::MachO);
  unsigned Runs = 0;
  for (const auto *Opt : {kUnoptimized, kOptimize}) {
    for (bool Old : {false, true}) {
      SCOPED_TRACE(Opt);
      SCOPED_TRACE(Old);
      const auto Compile = [&](llvm::ArrayRef<llvm::StringRef> Args) {
        llvm::SmallVector<llvm::StringRef> Cmd{kTLSCxx, kTargetFlag, kTarget,
                                               Opt};
        if (Old)
          Cmd.push_back(kTLSWithOriginal);
        Cmd.append(Args.begin(), Args.end());
        return llvm::sys::ExecuteAndWait(kTLSCxx, Cmd);
      };
      ASSERT_EQ(Compile({kDylib, Base, kOut, Input}), 0);
      ASSERT_EQ(Compile({kDylib, kTLSReference, Base, Source, kOut, Reference}),
                0);
      ASSERT_EQ(Compile({DriverSource, kOut, Driver}), 0);
      // Recent ld64 versions turn this obsolete section into S_REGULAR.
      // Restore its historical type while retaining the real linked fixups.
      if (Old) {
        auto File = llvm::MemoryBuffer::getFile(Input);
        ASSERT_TRUE(bool(File));
        auto Obj = llvm::object::ObjectFile::createObjectFile(
            (*File)->getMemBufferRef());
        ASSERT_TRUE(bool(Obj)) << llvm::toString(Obj.takeError());
        const auto &M = llvm::cast<llvm::object::MachOObjectFile>(**Obj);
        std::optional<uint64_t> FlagsAt;
        for (const auto &LC : M.load_commands()) {
          if (LC.C.cmd != llvm::MachO::LC_SEGMENT_64)
            continue;
          const auto Segment = M.getSegment64LoadCommand(LC);
          for (unsigned I = 0; I != Segment.nsects; ++I) {
            const auto Sec = M.getSection64(LC, I);
            if (llvm::StringRef(Sec.sectname, sizeof(Sec.sectname))
                    .rtrim(char(0)) == kLegacyInitSection)
              FlagsAt = LC.Ptr - (*File)->getBufferStart() + sizeof(Segment) +
                        I * sizeof(Sec) +
                        offsetof(llvm::MachO::section_64, flags);
          }
        }
        ASSERT_TRUE(FlagsAt);
        std::string Data = (*File)->getBuffer().str();
        auto *Flags = reinterpret_cast<uint8_t *>(Data.data()) + *FlagsAt;
        writeLE<uint32_t>(
            Flags, (readLE<uint32_t>(Flags) & ~llvm::MachO::SECTION_TYPE) |
                       llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS);
        std::ofstream(Input, std::ios::binary).write(Data.data(), Data.size());
        ASSERT_EQ(llvm::sys::ExecuteAndWait(
                      kCodesign, {kCodesign, kRemoveSignature, Input}),
                  0);
      }
      auto Original = Reader->load(Input);
      ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
      const auto *Entry = Original->findSymbol(kTLVSymbol);
      ASSERT_TRUE(Entry);
      const SourcePatchMapping Map{kTLVMapping, Entry->Addr};
      ASSERT_EQ(Compile({kTLVCompile, Source, kOut, Object}), 0);
      auto Bytes = llvm::MemoryBuffer::getFile(Object);
      ASSERT_TRUE(bool(Bytes));
      auto Patch = installMachOObjectPatch((*Bytes)->getBuffer(), *Original,
                                           {Map}, {}, Output, {});
      ASSERT_TRUE(bool(Patch)) << llvm::toString(Patch.takeError());
      auto Published = Reader->load(Output);
      ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());
      unsigned Anchors = 0;
      for (const auto &S : Published->Sections) {
        auto Kind = S.Type & llvm::MachO::SECTION_TYPE;
        EXPECT_NE(Kind, llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS);
        if (Kind != llvm::MachO::S_THREAD_LOCAL_VARIABLES)
          continue;
        EXPECT_EQ(S.Size, kTLVDescriptorWords * sizeof(uint64_t));
        auto Slot = Published->MachOAuthenticatedPointers.find(S.VA);
        ASSERT_NE(Slot, Published->MachOAuthenticatedPointers.end());
        EXPECT_EQ(Slot->second.Symbol, kTLVResolverSymbol);
        ++Anchors;
      }
      EXPECT_EQ(Anchors, 1u);
      auto Trace = llvm::find_if(Published->Exports, [](const auto &E) {
        return E.Name == kTraceSymbol;
      });
      ASSERT_NE(Trace, Published->Exports.end());
      auto Resolver = Published->MachOAuthenticatedPointers.find(Trace->Addr);
      ASSERT_NE(Resolver, Published->MachOAuthenticatedPointers.end());
      EXPECT_TRUE(Resolver->second.Symbol.empty());
      EXPECT_TRUE(Published->isCodeAddress(Resolver->second.Target));
      if (std::getenv(kExecute)) {
        if (Old)
          ASSERT_EQ(llvm::sys::ExecuteAndWait(
                        kCodesign, {kCodesign, kSign, kAdHocIdentity, Output}),
                    0);
        for (const auto &Image : {Reference, Output}) {
          SCOPED_TRACE(Image);
          ASSERT_EQ(llvm::sys::ExecuteAndWait(
                        Driver, {Driver, Image, std::to_string(Old)}),
                    0);
          ++Runs;
        }
      }
      ASSERT_TRUE(std::filesystem::remove(Output));
      ASSERT_EQ(Compile({kInvalidTLSInit, kTLVCompile, Source, kOut, Object}),
                0);
      Bytes = llvm::MemoryBuffer::getFile(Object);
      ASSERT_TRUE(bool(Bytes));
      auto Invalid = installMachOObjectPatch((*Bytes)->getBuffer(), *Original,
                                             {Map}, {}, Output, {});
      ASSERT_FALSE(bool(Invalid));
      llvm::consumeError(Invalid.takeError());
      EXPECT_FALSE(std::filesystem::exists(Output));
    }
  }
  RecordProperty(kPerThreadProperty, Runs);
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

TEST(AArch64LiftedImage, PublishedThreadLocalAccessUsesTheBoundDescriptor) {
#if defined(__APPLE__) && defined(__aarch64__)
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kTemporary, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kSource).string();
  const auto ProviderSource = (Root / kTLVProviderSource).string();
  const auto Provider = (Root / kTLVProvider).string();
  const auto DriverSource = (Root / kDriverSource).string(),
             Driver = (Root / kDriver).string();
  const auto Input = (Root / kOriginal).string(),
             Output = (Root / kOutput).string();
  const auto Object = (Root / kTLVObject).string(),
             Bad = (Root / kTLVAssembly).string();
  std::ofstream(Source) << kTLVBody;
  std::ofstream(ProviderSource) << kTLVProviderBody;
  std::ofstream(DriverSource) << kTLVDriver;
  std::ofstream(Bad) << kTLVBadRelocation;
  auto Reader = Loader::create(BinaryFormat::MachO);
  unsigned Calls = 0;
  for (const auto *Opt : {kUnoptimized, kOptimize}) {
    SCOPED_TRACE(Opt);
    const auto Compile = [&](llvm::ArrayRef<llvm::StringRef> Flags) {
      llvm::SmallVector<llvm::StringRef> Args{kCompiler, kTargetFlag, kTarget,
                                              Opt};
      Args.append(Flags.begin(), Flags.end());
      return llvm::sys::ExecuteAndWait(kCompiler, Args);
    };
    ASSERT_EQ(Compile({kDylib, ProviderSource, kOut, Provider}), 0);
    for (bool Local : {false, true}) {
      SCOPED_TRACE(Local);
      ASSERT_EQ(Compile({kDylib, Source, Local ? ProviderSource : Provider,
                         kOut, Input}),
                0);
      auto Original = Reader->load(Input);
      ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
      const auto *Entry = Original->findSymbol(kTLVSymbol);
      ASSERT_NE(Entry, nullptr);
      SourcePatchMapping Map{kTLVMapping, Entry->Addr};
      const auto Install = [&]() -> llvm::Expected<PatchResult> {
        auto Bytes = llvm::MemoryBuffer::getFile(Object);
        if (!Bytes)
          return llvm::errorCodeToError(Bytes.getError());
        return installMachOObjectPatch((*Bytes)->getBuffer(), *Original, {Map},
                                       {}, Output, {});
      };
      ASSERT_EQ(Compile({kTLVReplacement, kTLVCompile, Source, kOut, Object}),
                0);
      auto Patch = Install();
      ASSERT_TRUE(bool(Patch)) << llvm::toString(Patch.takeError());
      ASSERT_EQ(Patch->PatchedFunctions.size(), 1u);
      auto Published = Reader->load(Output);
      ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());
      if (Local) {
        const auto *Descriptor = Original->findSymbol(kTLVBind);
        ASSERT_NE(Descriptor, nullptr);
        const auto *Relocated = Published->findSymbol(kTLVBind);
        ASSERT_NE(Relocated, nullptr);
        EXPECT_EQ(Relocated->Addr,
                  Descriptor->Addr + Patch->OriginalSectionSlide);
        const auto Before =
            Original->MachOAuthenticatedPointers.find(Descriptor->Addr);
        const auto After =
            Published->MachOAuthenticatedPointers.find(Relocated->Addr);
        ASSERT_NE(Before, Original->MachOAuthenticatedPointers.end());
        ASSERT_NE(After, Published->MachOAuthenticatedPointers.end());
        EXPECT_EQ(After->second.Symbol, Before->second.Symbol);
        EXPECT_EQ(After->second.LibraryOrdinal, Before->second.LibraryOrdinal);
        EXPECT_EQ(After->second.Key, Before->second.Key);
      } else {
        const auto Slot =
            llvm::find_if(Original->DyldBindSlots, [](const auto &B) {
              return B.second.Name == kTLVBind;
            });
        ASSERT_NE(Slot, Original->DyldBindSlots.end());
        const auto New = Published->DyldBindSlots.find(
            Slot->first + Patch->OriginalSectionSlide);
        ASSERT_NE(New, Published->DyldBindSlots.end());
        EXPECT_EQ(New->second.Name, Slot->second.Name);
        EXPECT_EQ(New->second.LibraryOrdinal, Slot->second.LibraryOrdinal);
      }
      if (std::getenv(kExecute)) {
        ASSERT_EQ(Compile({DriverSource, kOut, Driver}), 0);
        for (bool Installed : {false, true}) {
          ASSERT_EQ(llvm::sys::ExecuteAndWait(
                        Driver, {Driver, Installed ? Output : Input,
                                 std::to_string(
                                     Installed ? kTLVChangedAdjustment : 0)}),
                    0);
          Calls += kTLVAccessesPerProcess;
        }
      }
      ASSERT_TRUE(std::filesystem::remove(Output));
      for (auto Flags :
           {std::vector<llvm::StringRef>{kTLVMissing, kTLVCompile, Source, kOut,
                                         Object},
            std::vector<llvm::StringRef>{kTLVCompile, Bad, kOut, Object}}) {
        ASSERT_EQ(Compile(Flags), 0);
        auto Refused = Install();
        ASSERT_FALSE(bool(Refused));
        llvm::consumeError(Refused.takeError());
        EXPECT_FALSE(std::filesystem::exists(Output));
      }
      if (Local) {
        std::ofstream(ProviderSource) << kTLVOrdinaryData;
        ASSERT_EQ(Compile({kDylib, kTLVOrdinaryFlag, Source, ProviderSource,
                           kOut, Input}),
                  0);
        std::ofstream(ProviderSource) << kTLVProviderBody;
        Original = Reader->load(Input);
        ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
        const auto *OtherEntry = Original->findSymbol(kTLVSymbol);
        ASSERT_NE(OtherEntry, nullptr);
        ASSERT_TRUE(Original->findSymbol(kTLVBind));
        Map.OriginalVA = OtherEntry->Addr;
        ASSERT_EQ(Compile({kTLVReplacement, kTLVCompile, Source, kOut, Object}),
                  0);
        auto Refused = Install();
        ASSERT_FALSE(bool(Refused));
        llvm::consumeError(Refused.takeError());
        EXPECT_FALSE(std::filesystem::exists(Output));
      }
    }
  }
  RecordProperty(kTLVCalls, Calls);
#else
  GTEST_SKIP() << kUnavailable;
#endif
}

} // namespace
