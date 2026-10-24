//===- RegistrationCxxCallEffectsTests.cpp - Current PE32 callees --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep a preserved call's physical ABI separate from current memory effects.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationCxxCall.h"
#include "../../../lib/backend/llvm/eh/MedLLVMEHHelpers.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/RegistrationCallEffects.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Entry = 0x401000;
constexpr va_t DataVA = 0x403000;
constexpr va_t TableVA = 0x404000;

TEST(RegistrationCxxCallEffects, ExternalDeclarationsRequireExactConvention) {
  llvm::LLVMContext Context;
  llvm::Module Module("cxx-call-conventions", Context);
  auto *Type =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {llvm::PointerType::get(Context, 0)}, false);
  constexpr llvm::CallingConv::ID Conventions[] = {
      llvm::CallingConv::C, llvm::CallingConv::X86_ThisCall,
      llvm::CallingConv::X86_StdCall, llvm::CallingConv::X86_FastCall};
  for (auto Convention : Conventions)
    EXPECT_TRUE(med_llvm_eh::canMaterializeExternalFunctionDeclaration(
        Module, "callee", Type, Convention));
  auto *Callee = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "callee", Module);
  for (auto Convention : Conventions) {
    Callee->setCallingConv(Convention);
    for (auto Expected : Conventions)
      EXPECT_EQ(med_llvm_eh::canMaterializeExternalFunctionDeclaration(
                    Module, "callee", Type, Expected),
                Convention == Expected);
    EXPECT_EQ(med_llvm_eh::canMaterializeExternalFunctionDeclaration(
                  Module, "callee", Type),
              Convention == llvm::CallingConv::C);
  }
}

struct CurrentCalleeFixture {
  enum class Kind { Leaf, Initializer, Throw, Rethrow };
  BinaryImage Image;
  LowFunc Source;
  llvm::LLVMContext Context;
  llvm::Module Module{"current-callee", Context};
  llvm::IRBuilder<> B{Context};
  llvm::CallInst *Call = nullptr;
  unsigned ReadOperand = 0;
  unsigned WriteOperand = 0;
  unsigned PCOperand = 0;
  unsigned ReturnOffset = 0;

  CurrentCalleeFixture(Kind Which) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Segment Text;
    Text.VA = Entry;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    auto &Bytes = Text.Data;
    if (Which == Kind::Initializer)
      Bytes = {0xc7, 1, 7, 0, 0, 0}; // mov dword ptr [ecx], 7
    const bool IsThrow = Which == Kind::Throw || Which == Kind::Rethrow;
    if (IsThrow)
      Bytes = {0x55, 0x8b, 0xec, 0x83, 0xec, 4, 0xc7, 0x45, 0xfc, 7, 0, 0, 0};
    auto Absolute = [&](uint8_t Opcode, uint32_t VA) {
      Bytes.push_back(Opcode);
      const auto At = Bytes.size();
      Bytes.resize(At + 4);
      writeLE<uint32_t>(Bytes.data() + At, VA);
      return At;
    };
    ReadOperand = Absolute(0xa1, DataVA);
    WriteOperand = Absolute(0xa3, DataVA + 4);
    if (IsThrow) {
      Bytes.insert(Bytes.end(),
                   {0x8b, 0x45, 4}); // Observe the physical caller PC.
      PCOperand = Absolute(0xa3, DataVA + 8);
      if (Which == Kind::Rethrow)
        Bytes.insert(Bytes.end(), {0x6a, 0, 0x6a, 0, 0xe8});
      else {
        Absolute(0x68, TableVA);
        Bytes.insert(Bytes.end(), {0x8d, 0x4d, 0xfc, 0x51, 0xe8});
      }
      const auto At = Bytes.size();
      Bytes.resize(At + 4);
      writeLE<uint32_t>(Bytes.data() + At, 0x80 - (At + 4));
      Bytes.insert(Bytes.end(), {0x90, 0x8b, 0xe5, 0x5d, 0xc3});
    } else {
      if (Which == Kind::Initializer)
        Bytes.insert(Bytes.end(),
                     {0x8b, 0xc1}); // Return the initialized receiver.
      else
        Bytes.insert(Bytes.end(), {0x31, 0xc0});
      ReturnOffset = Bytes.size();
      Bytes.push_back(0xc3);
    }
    Bytes.resize(0x100, 0xcc);
    Bytes[0x80] = 0xff;
    Bytes[0x81] = 0x25;
    writeLE<uint32_t>(Bytes.data() + 0x82, DataVA + 0x30);
    Text.Size = Text.FileSz = Bytes.size();
    Image.Segments.push_back(Text);
    Section Code;
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    Image.Sections.push_back(Code);
    Segment Data;
    Data.VA = DataVA;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(Data.Size);
    Data.Data[0x88] = '.';
    Data.Data[0x89] = 'H';
    Image.Segments.push_back(Data);
    Segment Table;
    Table.VA = TableVA;
    Table.Size = Table.FileSz = 0x80;
    Table.Flags = SegmentFlags::Readable;
    Table.Data.resize(Table.Size);
    auto Word = [&](unsigned At, uint32_t Value) {
      writeLE<uint32_t>(Table.Data.data() + At, Value);
    };
    Word(12, TableVA + 0x20);
    Word(0x20, 1);
    Word(0x24, TableVA + 0x30);
    Word(0x30, 1);
    Word(0x34, DataVA + 0x80);
    Word(0x3c, UINT32_MAX);
    Word(0x44, 4);
    Image.Segments.push_back(Table);
    Import Import;
    Import.Module = "VCRUNTIME140.dll";
    Import.Name = "_CxxThrowException";
    Import.IATAddr = DataVA + 0x30;
    Image.Imports.push_back(Import);
    auto &Op = Source.Blocks.emplace_back().Ops.emplace_back();
    Op.Opcode = NdOp::CALL;
    Op.NumInputs = 1;
    Op.Inputs[0] = NdVar::cst(Entry, 4);

    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    auto *Parent = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Parent));
    std::vector<llvm::Value *> Args;
    if (Which == Kind::Initializer) {
      auto *Object = B.CreateAlloca(B.getInt32Ty());
      Args.push_back(B.CreatePtrToInt(Object, B.getInt32Ty()));
    }
    std::vector<llvm::Type *> Types(Args.size(), B.getInt32Ty());
    auto Callee = Module.getOrInsertFunction(
        "callee", llvm::FunctionType::get(B.getInt32Ty(), Types, false));
    Call = B.CreateCall(Callee, Args);
    B.CreateRetVoid();
  }

  std::optional<RegistrationCalleeFrameContract> contract() {
    RegistrationCallCalleeIndex Index(Image);
    auto Contracts = Index.contracts(Source);
    if (!Contracts || Contracts->size() != 1)
      return std::nullopt;
    return Contracts->front();
  }
  bool accepts(const RegistrationCalleeFrameContract &Contract) {
    RegistrationCallFrameEffect Effect;
    Effect.Target = Contract.Target;
    Effect.StackPopBytes = Contract.StackPopBytes;
    Effect.DoesNotReturn = Contract.DoesNotReturn;
    if (Contract.isInitializer())
      Effect.ECXFrameOffset = -32;
    size_t Work = 0;
    return med_llvm_eh::getCheckedRegistrationCxxCallABI(
               Image, Contract, Effect, *Call, Work, true)
        .has_value();
  }
  void changeOperand(unsigned At, va_t VA) {
    writeLE<uint32_t>(Image.Segments[0].Data.data() + At, VA);
  }
};

TEST(RegistrationCxxCallEffects, RechecksCurrentImageReadsAndWrites) {
  for (auto Kind :
       {CurrentCalleeFixture::Kind::Leaf,
        CurrentCalleeFixture::Kind::Initializer,
        CurrentCalleeFixture::Kind::Throw, CurrentCalleeFixture::Kind::Rethrow})
    for (bool Write : {false, true}) {
      SCOPED_TRACE(int(Kind));
      SCOPED_TRACE(Write);
      CurrentCalleeFixture F(Kind);
      ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
      const auto Saved = F.contract();
      ASSERT_TRUE(Saved);
      ASSERT_EQ(Saved->isInitializer(),
                Kind == CurrentCalleeFixture::Kind::Initializer);
      ASSERT_EQ(Saved->isThrow(),
                Kind == CurrentCalleeFixture::Kind::Throw ||
                    Kind == CurrentCalleeFixture::Kind::Rethrow);
      ASSERT_TRUE(F.accepts(*Saved));
      F.changeOperand(Write ? F.WriteOperand : F.ReadOperand, DataVA + 16);
      const auto Current = F.contract();
      ASSERT_TRUE(Current);
      ASSERT_TRUE(F.accepts(*Current));
      EXPECT_FALSE(F.accepts(*Saved));
    }
}

TEST(RegistrationCxxCallEffects, InputPE32CxxParentRechecksNativeCallEffects) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_CLEANUP_ONLY_PE32");
  if (!Path)
    GTEST_SKIP() << "set the authored shared C++ cleanup image";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Export = llvm::find_if(Image->Exports, [](const auto &E) {
    return E.Name == "cleanup_only_parent";
  });
  ASSERT_NE(Export, Image->Exports.end());
  const va_t Target = Export->Addr;
  const auto *EH = Image->ExceptionMetadata.findFunction(Target);
  ASSERT_TRUE(EH && EH->Cxx && EH->Registration);
  if (EH->Registration->RealignedFrame)
    GTEST_SKIP() << "this callee contract requires a non-realigned parent";
  CurrentCalleeFixture F(CurrentCalleeFixture::Kind::Leaf);
  F.Image = std::move(*Image);
  const auto Saved = getCheckedX86RegistrationCxxCalleeABI(F.Image, Target);
  ASSERT_TRUE(Saved);
  ASSERT_TRUE(Saved->isCxxParent());
  ASSERT_TRUE(Saved->HasIndependentScalarReturn);
  ASSERT_FALSE(Saved->ImageReads.empty());
  ASSERT_FALSE(Saved->ImageWrites.empty());
  ASSERT_FALSE(Saved->RequiredImports.empty());
  ASSERT_FALSE(Saved->RuntimeThrowInfos.empty());
  ASSERT_TRUE(F.accepts(*Saved));
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = *Saved;
    switch (Mutation) {
    case 0:
      Changed.ImageReads.clear();
      break;
    case 1:
      Changed.ImageWrites.clear();
      break;
    case 2:
      Changed.CodeRanges.clear();
      break;
    case 3:
      Changed.RequiredImports.clear();
      break;
    case 4:
      Changed.RuntimeThrowInfos.clear();
      break;
    case 5:
      Changed.HasIndependentScalarReturn = false;
      break;
    case 6:
      Changed.CalleeKind = RegistrationCalleeFrameContract::Kind::Leaf;
      break;
    }
    EXPECT_FALSE(F.accepts(Changed));
  }
  for (auto &Import : F.Image.Imports)
    if (std::find(Saved->RequiredImports.begin(), Saved->RequiredImports.end(),
                  Import.IATAddr) != Saved->RequiredImports.end())
      Import.Module = "unknown-provider.dll";
  EXPECT_FALSE(F.accepts(*Saved));
}

TEST(RegistrationCxxCallEffects, RechecksPrivateThrowCallerPCWrites) {
  CurrentCalleeFixture F(CurrentCalleeFixture::Kind::Throw);
  const auto Saved = F.contract();
  ASSERT_TRUE(Saved);
  ASSERT_FALSE(Saved->CallerPCWrites.empty());
  ASSERT_TRUE(F.accepts(*Saved));
  F.changeOperand(F.PCOperand, DataVA + 20);
  const auto Current = F.contract();
  ASSERT_TRUE(Current);
  EXPECT_TRUE(F.accepts(*Current));
  EXPECT_FALSE(F.accepts(*Saved));
}

TEST(RegistrationCxxCallEffects, RechecksTheCompletePreservedCodeExtent) {
  for (auto Kind : {CurrentCalleeFixture::Kind::Leaf,
                    CurrentCalleeFixture::Kind::Initializer}) {
    SCOPED_TRACE(int(Kind));
    CurrentCalleeFixture F(Kind);
    const auto Saved = F.contract();
    ASSERT_TRUE(Saved);
    ASSERT_TRUE(F.accepts(*Saved));
    F.Image.Segments[0].Data[F.ReturnOffset] = 0x90;
    F.Image.Segments[0].Data[F.ReturnOffset + 1] = 0xc3;
    const auto Current = F.contract();
    ASSERT_TRUE(Current);
    EXPECT_TRUE(F.accepts(*Current));
    EXPECT_FALSE(F.accepts(*Saved));
  }
}

TEST(RegistrationCxxCallEffects, RechecksPrivateThrownType) {
  CurrentCalleeFixture F(CurrentCalleeFixture::Kind::Throw);
  const auto Saved = F.contract();
  ASSERT_TRUE(Saved);
  ASSERT_TRUE(F.accepts(*Saved));
  F.Image.Segments[1].Data[0x98] = '.';
  F.Image.Segments[1].Data[0x99] = 'H';
  writeLE<uint32_t>(F.Image.Segments[2].Data.data() + 0x34, DataVA + 0x90);
  const auto Current = F.contract();
  ASSERT_TRUE(Current);
  ASSERT_NE(Current->ThrownTypeVA, Saved->ThrownTypeVA);
  EXPECT_TRUE(F.accepts(*Current));
  EXPECT_FALSE(F.accepts(*Saved));
}

TEST(RegistrationCxxCallEffects, RechecksTransitiveMemberImageWrites) {
  CurrentCalleeFixture F(CurrentCalleeFixture::Kind::Leaf);
  auto &Bytes = F.Image.Segments[0].Data;
  std::copy_n(Bytes.begin(), F.ReturnOffset + 1, Bytes.begin() + 0x40);
  const std::vector<uint8_t> Parent{0x55, 0x8b, 0xec, 0xe8, 0x38,
                                    0,    0,    0,    0x5d, 0xc3};
  std::copy(Parent.begin(), Parent.end(), Bytes.begin());
  const auto Saved = F.contract();
  ASSERT_TRUE(Saved);
  ASSERT_TRUE(F.accepts(*Saved));
  F.changeOperand(0x40 + F.WriteOperand, DataVA + 16);
  const auto Current = F.contract();
  ASSERT_TRUE(Current);
  EXPECT_TRUE(F.accepts(*Current));
  EXPECT_FALSE(F.accepts(*Saved));
}

TEST(RegistrationCxxCallEffects, PrivateProofDoesNotCertifyRuntimeImport) {
  CurrentCalleeFixture F(CurrentCalleeFixture::Kind::Throw);
  auto Saved = F.contract();
  ASSERT_TRUE(Saved);
  const auto Proof = getCheckedX86RegistrationThrowCalleeABI(F.Image, Entry);
  ASSERT_TRUE(Proof);
  size_t Work = 0;
  ASSERT_TRUE(hasSameX86RegistrationThrowEffects(*Saved, *Proof, Work));
  Saved->CalleeKind = RegistrationCalleeFrameContract::Kind::RuntimeThrow;
  EXPECT_FALSE(hasSameX86RegistrationThrowEffects(*Saved, *Proof, Work));
}

TEST(RegistrationCxxCallEffects, ChargesEveryComparedExtentToSharedBudget) {
  for (auto Kind :
       {CurrentCalleeFixture::Kind::Leaf, CurrentCalleeFixture::Kind::Throw}) {
    SCOPED_TRACE(int(Kind));
    CurrentCalleeFixture F(Kind);
    const auto Saved = F.contract();
    ASSERT_TRUE(Saved);
    auto Leaf = getCheckedX86RegistrationMemberCalleeABI(
        F.Image, Entry, nullptr, RegistrationLeafResult::Discarded);
    auto Throw = getCheckedX86RegistrationThrowCalleeABI(F.Image, Entry);
    ASSERT_TRUE(bool(Leaf) != bool(Throw));
    auto Matches = [&](size_t &Work) {
      return Leaf ? hasSameX86RegistrationLeafEffects(*Saved, *Leaf, Work)
                  : hasSameX86RegistrationThrowEffects(*Saved, *Throw, Work);
    };
    size_t Work = 0;
    ASSERT_TRUE(Matches(Work));
    const auto Cost = Work;
    ASSERT_GT(Cost, Saved->CodeRanges.size() + Saved->ImageReads.size() +
                        Saved->ImageWrites.size() +
                        Saved->CallerPCWrites.size());
    Work = limits::kMaxRegistrationEHStateWork - Cost;
    EXPECT_TRUE(Matches(Work));
    EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
    Work = limits::kMaxRegistrationEHStateWork - Cost + 1;
    EXPECT_FALSE(Matches(Work));
    EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
    EXPECT_FALSE(Matches(Work));
  }
}
} // namespace
