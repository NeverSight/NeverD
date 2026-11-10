//===- AArch64EntryABITests.cpp - Lifted entry ABI tests
//-------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent native callers for logical-to-physical ABI reconstruction.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/AArch64/LLVMAArch64ReturnProtection.h"
#include "neverd/backend/llvm/AArch64/LLVMEntryABI.h"
#include "neverd/backend/llvm/LLVMSourceCallValue.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace neverd;
#define A64_ENTRY_TEXT(Name, Text) constexpr char Name[] = Text;
#define A64_ENTRY_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64EntryABIFixtures.def"

TypeRef record(TypeRef Element, unsigned Count) {
  return NdType::makeStruct(std::vector<TypeRef>(Count, Element));
}

struct Fixture {
  MedFunc Source;
  llvm::Function *Body = nullptr;
};

// The body takes one raw value per observed physical carrier, deliberately
// reversed. The independent C caller uses source order and native registers.
Fixture identity(llvm::Module &Module, llvm::StringRef Name, TypeRef Type,
                 unsigned Prefix = 0, TypeRef PrefixType = nullptr) {
  Fixture F;
  F.Source.Entry = kEntryVA;
  F.Source.Name = Name.str();
  auto &Hint = F.Source.SourceTypeHint.emplace();
  Hint.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  if (!sourceAArch64VectorAggregateMembers(Type).empty() ||
      isSourceAArch64VectorType(Type))
    Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.ReturnType = Type;
  for (unsigned I = 0; I < Prefix; ++I)
    Hint.Parameters.push_back(
        {kParameter,
         PrefixType ? PrefixType : NdType::makeInt(kWordBytes, false)});
  Hint.Parameters.push_back({kParameter, Type});
  std::string Diagnostic;
  EXPECT_TRUE(assignDarwinFixedSourceABI(Hint, Arch::AArch64, Diagnostic))
      << Diagnostic;
  const auto Bindings = sourceABIParameters(Hint);
  const bool Indirect =
      Hint.ReturnLocation.Kind == SourceABICarrierKind::IndirectResultPointer;
  std::vector<SourceABIValueLocation> Locations;
  for (const auto &Binding : Bindings)
    Locations.push_back(Binding.Location);
  if (Indirect)
    Locations.push_back(Hint.ReturnLocation);
  std::reverse(Locations.begin(), Locations.end());
  std::vector<llvm::Type *> Parameters;
  const auto Stack = getTargetRegInfo(Arch::AArch64)
                         .integerArgumentLayout(BinaryFormat::MachO);
  for (const auto &Location : Locations) {
    MedVar P;
    P.Kind = MedVar::Param;
    P.TheArch = Arch::AArch64;
    P.Size = Location.ValueBytes;
    P.RegOff = Location.Kind == SourceABICarrierKind::Stack
                   ? kNoParamReg
                   : Location.RegisterOffset;
    P.Id = P.RegOff == kNoParamReg
               ? Stack.Registers.size() +
                     (Location.EntryStackOffset - Stack.EntryStackBase) /
                         Stack.SlotBytes
               : F.Source.Params.size();
    F.Source.Params.push_back(P);
    Parameters.push_back(
        llvm::IntegerType::get(Module.getContext(), P.Size * kByteBits));
  }
  auto Returns = Hint.ReturnComponents;
  if (Returns.empty() && !Indirect)
    Returns.push_back(Hint.ReturnLocation);
  std::vector<llvm::Type *> Fields;
  for (const auto &Location : Returns) {
    Fields.push_back(llvm::IntegerType::get(Module.getContext(),
                                            Location.ValueBytes * kByteBits));
    F.Source.MultiReturn.push_back(
        {Location.RegisterOffset, Location.ValueBytes,
         Location.Kind == SourceABICarrierKind::FloatingRegister});
  }
  llvm::Type *ResultType =
      Indirect ? llvm::Type::getVoidTy(Module.getContext())
               : llvm::StructType::get(Module.getContext(), Fields);
  F.Body = llvm::Function::Create(
      llvm::FunctionType::get(ResultType, Parameters, false),
      llvm::GlobalValue::ExternalLinkage, Name, Module);
  rewrite_source::setOriginalVA(*F.Body, F.Source.Entry);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Module.getContext(), kBody, F.Body));
  auto Find = [&](const SourceABIValueLocation &Location) {
    for (unsigned I = 0; I < Locations.size(); ++I)
      if (Location.Kind == Locations[I].Kind &&
          (Location.Kind == SourceABICarrierKind::Stack
               ? Location.EntryStackOffset == Locations[I].EntryStackOffset
               : Location.RegisterOffset == Locations[I].RegisterOffset))
        return F.Body->getArg(I);
    ADD_FAILURE();
    return F.Body->getArg(0);
  };
  if (Indirect) {
    auto *Pointer = llvm::PointerType::getUnqual(Module.getContext());
    auto *Input =
        Builder.CreateIntToPtr(Find(Bindings.back().Location), Pointer);
    auto *Output = Builder.CreateIntToPtr(Find(Hint.ReturnLocation), Pointer);
    Builder.CreateMemCpy(Output, llvm::MaybeAlign(), Input, llvm::MaybeAlign(),
                         Type->Size);
    Builder.CreateRetVoid();
  } else {
    llvm::Value *Result = llvm::PoisonValue::get(ResultType);
    std::vector<llvm::Value *> Parts;
    for (const auto &Binding : Bindings)
      if (Binding.ParameterIndex == Prefix)
        Parts.push_back(Find(Binding.Location));
    for (unsigned I = 0; I < Parts.size(); ++I) {
      // Rotate records so an unimplemented identity entry cannot pass by
      // merely leaving the argument/return bank untouched.
      const unsigned From =
          Type->Kind == NdTypeKind::Struct ? (I + 1) % Parts.size() : I;
      Result = Builder.CreateInsertValue(Result, Parts[From], I);
    }
    Builder.CreateRet(Result);
  }
  return F;
}

void addFixtures(llvm::Module &Module) {
  auto Vector = [](unsigned Bytes) {
    auto Type = NdType::makeInt(Bytes, false);
    Type->SourceName = Bytes == kWordBytes ? kSourceAArch64Vector64CType
                                           : kSourceAArch64Vector128CType;
    return Type;
  };
  const auto Add = [&](llvm::StringRef Name, TypeRef Type, unsigned Prefix = 0,
                       TypeRef PrefixType = nullptr) {
    auto F = identity(Module, Name, Type, Prefix, PrefixType);
    auto Entry = reconstructDarwinAArch64Entry(*F.Body, F.Source);
    ASSERT_TRUE(bool(Entry)) << llvm::toString(Entry.takeError());
    EXPECT_EQ((*Entry)->getName(), Name);
    EXPECT_TRUE(F.Body->hasInternalLinkage());
    EXPECT_EQ(F.Body->getMetadata(rewrite_source::FunctionAttachment), nullptr);
    EXPECT_NE((*Entry)->getMetadata(rewrite_source::FunctionAttachment),
              nullptr);
    if (F.Source.SourceTypeHint->ReturnLocation.Kind ==
        SourceABICarrierKind::IndirectResultPointer) {
      EXPECT_TRUE((*Entry)->hasParamAttribute(0, llvm::Attribute::StructRet));
      EXPECT_FALSE((*Entry)->hasParamAttribute(0, llvm::Attribute::NoAlias));
      EXPECT_FALSE((*Entry)->hasParamAttribute(1, llvm::Attribute::ByVal));
    }
  };
  Add(kScalar, NdType::makeInt(kWordBytes, false));
  Add(kSigned, NdType::makeInt(sizeof(int8_t), true));
  Add(kUnsigned, NdType::makeInt(sizeof(uint8_t), false));
  Add(kSigned16, NdType::makeInt(sizeof(int16_t), true));
  Add(kUnsigned16, NdType::makeInt(sizeof(uint16_t), false));
  Add(kFloat, NdType::makeFloat(sizeof(float)));
  Add(kDouble, NdType::makeFloat(sizeof(double)));
  Add(kPointer, NdType::makePtr(NdType::makeInt(kWordBytes, false)));
  Add(kVector64, Vector(kWordBytes));
  Add(kVector128, Vector(kWideBytes));
  Add(kVectorStack, Vector(kWideBytes), kRegisterBankSize,
      NdType::makeFloat(sizeof(double)));
  Add(kWide, NdType::makeInt(kWideBytes, false), 1);
  Add(kWideStack, NdType::makeInt(kWideBytes, false), kRegisterBankSize);
  Add(kPair, record(NdType::makeInt(kWordBytes, false), 2));
  Add(kHFA, record(NdType::makeFloat(sizeof(double)), kHFAElements));
  Add(kHFAStack, record(NdType::makeFloat(sizeof(double)), kHFAElements),
      kRegisterBankSize, NdType::makeFloat(sizeof(double)));
  Add(kHVA64, record(Vector(kWordBytes), kHVAElements));
  Add(kHVA128, record(Vector(kWideBytes), kHVAElements));
  MedFunc Emitted;
  Emitted.Name = kEmittedHVA;
  Emitted.Entry = kEntryVA;
  auto &Hint = Emitted.SourceTypeHint.emplace();
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.ReturnType = record(Vector(kWideBytes), kHVAElements);
  Hint.Parameters = {{kParameter, Hint.ReturnType}};
  std::string EntryDiagnostic;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Hint, Arch::AArch64, EntryDiagnostic))
      << EntryDiagnostic;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Emitted.Entry;
  for (unsigned I = 0; I < kHVAElements; ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Arch::AArch64;
    Parameter.Id = I;
    Parameter.Size = kWideBytes;
    Parameter.RegOff = Hint.Parameters.front().Components[I].RegisterOffset;
    Emitted.Params.push_back(Parameter);
    Emitted.MultiReturn.push_back({Parameter.RegOff, Parameter.Size, true});
  }
  for (unsigned I = 0; I < kHVAElements; ++I) {
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = Emitted.Params[I];
    Copy.Output.Kind = MedVar::Reg;
    Copy.Output.SSAVer = 1;
    Copy.addInput(Emitted.Params[(I + 1) % kHVAElements]);
    Block.Ops.push_back(Copy);
  }
  MedOp ReturnValue;
  ReturnValue.Opcode = NdOp::RETURN;
  Block.Ops.push_back(ReturnValue);
  Emitted.Blocks.push_back(Block);
  MedLLVMEmitter Emitter;
  Emitter.setReconstructAArch64SourceEntries(true);
  auto Lifted = Emitter.emit({Emitted}, Module.getContext(), kEmittedHVA,
                             Arch::AArch64, {}, nullptr, BinaryFormat::MachO);
  ASSERT_NE(Lifted, nullptr);
  Lifted->setTargetTriple(Module.getTargetTriple());
  ASSERT_FALSE(llvm::Linker::linkModules(Module, std::move(Lifted)));
  Add(kLarge, record(NdType::makeFloat(sizeof(double)), kLargeElements));
  Add(kStack, NdType::makeInt(kWordBytes, false), kStackPrefix);
  auto Packed = identity(Module, kPacked, NdType::makeInt(kWordBytes, false));
  auto &PackedHint = *Packed.Source.SourceTypeHint;
  PackedHint.Parameters.clear();
  for (unsigned I = 0; I < kRegisterBankSize; ++I)
    PackedHint.Parameters.push_back(
        {kParameter, NdType::makeInt(kWordBytes, false)});
  for (unsigned I = 0; I < kWordBytes; ++I)
    PackedHint.Parameters.push_back(
        {kParameter, NdType::makeInt(sizeof(uint8_t), false)});
  std::string Diagnostic;
  ASSERT_TRUE(assignDarwinFixedSourceABI(PackedHint, Arch::AArch64, Diagnostic))
      << Diagnostic;
  Packed.Source.Params.front().RegOff = kNoParamReg;
  Packed.Source.Params.front().Id = kRegisterBankSize;
  auto PackedEntry = reconstructDarwinAArch64Entry(*Packed.Body, Packed.Source);
  ASSERT_TRUE(bool(PackedEntry)) << llvm::toString(PackedEntry.takeError());
  auto Throwing =
      identity(Module, kThrowing, NdType::makeInt(kWordBytes, false));
  auto *Return = llvm::cast<llvm::ReturnInst>(
      Throwing.Body->getEntryBlock().getTerminator());
  llvm::IRBuilder<> Builder(Return);
  auto *Type = llvm::FunctionType::get(Builder.getInt64Ty(),
                                       {Builder.getInt64Ty()}, false);
  auto *Call = Builder.CreateCall(Module.getOrInsertFunction(kThrow, Type),
                                  {Throwing.Body->getArg(0)});
  Return->setOperand(
      0, Builder.CreateInsertValue(
             llvm::PoisonValue::get(Throwing.Body->getReturnType()), Call, 0));
  Throwing.Body->setUWTableKind(llvm::UWTableKind::Async);
  auto Entry = reconstructDarwinAArch64Entry(*Throwing.Body, Throwing.Source);
  ASSERT_TRUE(bool(Entry)) << llvm::toString(Entry.takeError());
}

std::unique_ptr<llvm::Module> module(llvm::LLVMContext &Context) {
  auto Result = std::make_unique<llvm::Module>(kModule, Context);
  Result->setTargetTriple(llvm::Triple(kTriple));
  Result->setDataLayout(Result->getTargetTriple().computeDataLayout());
  return Result;
}

TEST(AArch64EntryABI, LogicalValuesReconstructAllSupportedCarrierFamilies) {
  llvm::LLVMContext Context;
  auto Module = module(Context);
  addFixtures(*Module);
  ASSERT_FALSE(HasFailure());
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto Object = Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO,
                                  kArm64eTriple);
  EXPECT_TRUE(Object.Success);
  EXPECT_FALSE(Object.ObjectData.empty());
}

TEST(AArch64EntryABI, AddressesUseNativeEntryAndDirectCallsKeepCarrierABI) {
  llvm::LLVMContext Context;
  auto Module = module(Context);
  auto F = identity(*Module, kScalar, NdType::makeInt(kWordBytes, false));
  auto *Saved = new llvm::GlobalVariable(*Module, F.Body->getType(), false,
                                         llvm::GlobalValue::ExternalLinkage,
                                         F.Body, kSaved);
  auto *SignedStorage = new llvm::GlobalVariable(
      *Module, F.Body->getType(), true, llvm::GlobalValue::ExternalLinkage,
      nullptr, kSigned);
  auto *Signed = llvm::ConstantPtrAuth::get(
      F.Body,
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context),
                             kAArch64PAuthKeyIA),
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), kEntryVA),
      SignedStorage,
      llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(Context)));
  SignedStorage->setInitializer(Signed);
  auto *Block = F.Body->getEntryBlock().splitBasicBlock(
      F.Body->getEntryBlock().begin(), kInterior);
  auto *Label = llvm::BlockAddress::get(F.Body, Block);
  auto *Interior = new llvm::GlobalVariable(*Module, Label->getType(), false,
                                            llvm::GlobalValue::ExternalLinkage,
                                            Label, kInterior);
  auto *Caller = llvm::Function::Create(F.Body->getFunctionType(),
                                        llvm::GlobalValue::ExternalLinkage,
                                        kCaller, *Module);
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, kBody, Caller));
  auto *Call = Builder.CreateCall(F.Body, {Caller->getArg(0)});
  Builder.CreateRet(Call);
  auto Entry = reconstructDarwinAArch64Entry(*F.Body, F.Source);
  ASSERT_TRUE(bool(Entry)) << llvm::toString(Entry.takeError());
  EXPECT_EQ(Call->getCalledFunction(), F.Body);
  EXPECT_EQ(Saved->getInitializer(), *Entry);
  const auto *NewSigned =
      llvm::cast<llvm::ConstantPtrAuth>(SignedStorage->getInitializer());
  EXPECT_EQ(NewSigned->getPointer(), *Entry);
  EXPECT_EQ(NewSigned->getAddrDiscriminator(), SignedStorage);
  EXPECT_EQ(NewSigned->getDiscriminator()->getZExtValue(), kEntryVA);
  EXPECT_EQ(
      llvm::cast<llvm::BlockAddress>(Interior->getInitializer())->getFunction(),
      F.Body);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
}

TEST(AArch64EntryABI, CanonicalCallsKeepTheirLogicalSignature) {
  llvm::LLVMContext Context;
  auto Module = module(Context);
  auto F = identity(*Module, kWide, NdType::makeInt(kWideBytes, false), 1);
  auto *Wide = llvm::IntegerType::get(Context, kWideBytes * kByteBits);
  auto *Word = llvm::IntegerType::get(Context, kWordBytes * kByteBits);
  auto *Type = llvm::FunctionType::get(Wide, {Word, Wide}, false);
  auto *Caller = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, kCaller, *Module);
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, kBody, Caller));
  auto *Call =
      Builder.CreateCall(Type, F.Body, {Caller->getArg(0), Caller->getArg(1)});
  Call->setMetadata(kAArch64SourceEntryCall, llvm::MDNode::get(Context, {}));
  Builder.CreateRet(Call);
  auto Entry = reconstructDarwinAArch64Entry(*F.Body, F.Source);
  ASSERT_TRUE(bool(Entry)) << llvm::toString(Entry.takeError());
  EXPECT_EQ(Call->getCalledFunction(), *Entry);
  EXPECT_EQ(Call->getFunctionType(), (*Entry)->getFunctionType());
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
}

TEST(AArch64EntryABI, MissingPhysicalCarrierRefusesWithoutMutation) {
  llvm::LLVMContext Context;
  auto Module = module(Context);
  auto F = identity(*Module, kScalar, NdType::makeInt(kWordBytes, false));
  F.Source.Params.front().RegOff += kWordBytes;
  std::string Before;
  llvm::raw_string_ostream Out(Before);
  Module->print(Out, nullptr);
  auto Entry = reconstructDarwinAArch64Entry(*F.Body, F.Source);
  ASSERT_FALSE(bool(Entry));
  llvm::consumeError(Entry.takeError());
  std::string After;
  llvm::raw_string_ostream AfterOut(After);
  Module->print(AfterOut, nullptr);
  EXPECT_EQ(Before, After);
}

TEST(AArch64EntryABI, UnusedAttributedCarriersStillRequireTheirValue) {
  for (bool Attributed : {false, true}) {
    llvm::LLVMContext Context;
    auto Module = module(Context);
    auto F = identity(*Module, kScalar, NdType::makeInt(kWordBytes, false));
    F.Source.Params.front().RegOff += kWordBytes;
    auto *Return =
        llvm::cast<llvm::ReturnInst>(F.Body->getEntryBlock().getTerminator());
    Return->setOperand(0,
                       llvm::Constant::getNullValue(F.Body->getReturnType()));
    // Remove the dead insertvalue so the argument is actually unused.
    Return->getPrevNode()->eraseFromParent();
    if (Attributed)
      F.Body->addParamAttr(0, llvm::Attribute::NoUndef);
    auto Entry = reconstructDarwinAArch64Entry(*F.Body, F.Source);
    if (Attributed) {
      ASSERT_FALSE(bool(Entry));
      llvm::consumeError(Entry.takeError());
      EXPECT_EQ(Module->size(), 1u);
    } else {
      ASSERT_TRUE(bool(Entry)) << llvm::toString(Entry.takeError());
    }
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  }
}

TEST(AArch64EntryABI, AggregatePhysicalResultsRefuseBeforeMutation) {
  llvm::LLVMContext Context;
  auto Module = module(Context);
  auto F = identity(*Module, kScalar, NdType::makeInt(kWordBytes, false));
  auto *Word = llvm::Type::getInt64Ty(Context);
  auto *Type =
      llvm::StructType::get(Context, {Word, llvm::ArrayType::get(Word, 1)});
  auto *Body = llvm::Function::Create(
      llvm::FunctionType::get(Type, {Word}, false),
      llvm::GlobalValue::ExternalLinkage, kCaller, *Module);
  rewrite_source::setOriginalVA(*Body, F.Source.Entry);
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, kBody, Body));
  Builder.CreateRet(llvm::Constant::getNullValue(Type));
  auto Extra = F.Source.MultiReturn.front();
  Extra.RegOff += kWordBytes;
  F.Source.MultiReturn.push_back(Extra);
  std::string Before;
  llvm::raw_string_ostream Out(Before);
  Module->print(Out, nullptr);
  auto Entry = reconstructDarwinAArch64Entry(*Body, F.Source);
  ASSERT_FALSE(bool(Entry));
  llvm::consumeError(Entry.takeError());
  std::string After;
  llvm::raw_string_ostream AfterOut(After);
  Module->print(AfterOut, nullptr);
  EXPECT_EQ(Before, After);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
}

TEST(AArch64EntryABI, InvokesKeepTheirExceptionalSuccessor) {
  llvm::LLVMContext Context;
  auto Module = module(Context);
  auto F = identity(*Module, kScalar, NdType::makeInt(kWordBytes, false));
  auto *Caller = llvm::Function::Create(F.Body->getFunctionType(),
                                        llvm::GlobalValue::ExternalLinkage,
                                        kCaller, *Module);
  auto *EntryBlock = llvm::BasicBlock::Create(Context, kBody, Caller);
  auto *Normal = llvm::BasicBlock::Create(Context, kScalar, Caller);
  auto *Unwind = llvm::BasicBlock::Create(Context, kThrowing, Caller);
  llvm::IRBuilder<> Builder(EntryBlock);
  auto Personality = Module->getOrInsertFunction(
      kPersonality, llvm::FunctionType::get(Builder.getInt32Ty(), true));
  Caller->setPersonalityFn(llvm::cast<llvm::Constant>(Personality.getCallee()));
  auto *Call =
      Builder.CreateInvoke(F.Body, Normal, Unwind, {Caller->getArg(0)});
  Builder.SetInsertPoint(Normal);
  Builder.CreateRet(Call);
  Builder.SetInsertPoint(Unwind);
  auto *Landing = Builder.CreateLandingPad(
      llvm::StructType::get(Builder.getPtrTy(), Builder.getInt32Ty()), 0);
  Landing->setCleanup(true);
  Builder.CreateResume(Landing);
  auto Entry = reconstructDarwinAArch64Entry(*F.Body, F.Source);
  ASSERT_TRUE(bool(Entry)) << llvm::toString(Entry.takeError());
  EXPECT_EQ(Call->getCalledFunction(), F.Body);
  EXPECT_EQ(Call->getUnwindDest(), Unwind);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
}

TEST(AArch64EntryABI, WrongResultBankAndIncompleteSignaturesRefuse) {
  for (unsigned Variant = 0; Variant != kRefusalVariants; ++Variant) {
    llvm::LLVMContext Context;
    auto Module = module(Context);
    auto F = identity(*Module, kScalar, NdType::makeFloat(sizeof(double)));
    switch (Variant) {
    case 0:
      F.Source.MultiReturn.front().IsFP = false;
      break;
    case 1:
      F.Source.SourceTypeHint.reset();
      break;
    case 2:
      F.Source.IsVariadic = true;
      break;
    case 3:
      F.Source.SourceTypeHint->HasExplicitABI = false;
      break;
    case 4:
      F.Source.SourceTypeHint->Parameters.front().Location.RegisterOffset +=
          kWideBytes;
      break;
    case 5:
      ++F.Source.Entry;
      break;
    }
    auto Entry = reconstructDarwinAArch64Entry(*F.Body, F.Source);
    ASSERT_FALSE(bool(Entry));
    llvm::consumeError(Entry.takeError());
    EXPECT_EQ(F.Body->getName(), kScalar);
    EXPECT_TRUE(F.Body->hasExternalLinkage());
    EXPECT_EQ(Module->size(), 1u);
  }
}

TEST(AArch64EntryABI, ReturnSigningAndUnwindPoliciesSurviveEntryProjection) {
  llvm::LLVMContext Context;
  auto Module = module(Context);
  auto F = identity(*Module, kScalar, NdType::makeInt(kWordBytes, false));
  F.Body->addFnAttr(kReturnSigningArm64eAttribute);
  F.Body->setUWTableKind(llvm::UWTableKind::Async);
  auto Entry = reconstructDarwinAArch64Entry(*F.Body, F.Source);
  ASSERT_TRUE(bool(Entry)) << llvm::toString(Entry.takeError());
  EXPECT_TRUE(F.Body->hasFnAttribute(kReturnSigningArm64eAttribute));
  EXPECT_TRUE((*Entry)->hasFnAttribute(kReturnSigningArm64eAttribute));
  EXPECT_EQ((*Entry)->getUWTableKind(), F.Body->getUWTableKind());
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto Repeated = reconstructDarwinAArch64Entry(*F.Body, F.Source);
  ASSERT_FALSE(bool(Repeated));
  llvm::consumeError(Repeated.takeError());
}

TEST(AArch64EntryABI,
     DecodedBodiesReconstructEntriesWithoutGrantingPublication) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.MachOCPUSubtype = kClassicSubtype;
  Segment Text;
  Text.VA = kEntryVA;
  Text.Size = Text.FileSz = sizeof(kAddReturn);
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  for (unsigned I = 0; I < std::size(kAddReturn); ++I)
    writeLE<uint32_t>(Text.Data.data() + I * sizeof(uint32_t), kAddReturn[I]);
  Image.Segments.push_back(std::move(Text));
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch));
  Decoder.setStrict(true);
  auto Low = CFGBuilder().build(Image, Decoder, kEntryVA, kScalar);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Function = Converter.convert(Low, Image.Arch, Image.Format);
  auto &Hint = Function.SourceTypeHint.emplace();
  Hint.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  Hint.ReturnType = NdType::makeInt(kWordBytes, false);
  Hint.Parameters = {{kParameter, Hint.ReturnType},
                     {kParameter, Hint.ReturnType}};
  std::string Diagnostic;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Hint, Image.Arch, Diagnostic))
      << Diagnostic;
  MedLLVMEmitter Emitter;
  Emitter.setReconstructAArch64SourceEntries(true);
  llvm::LLVMContext Context;
  auto Module = Emitter.emit({Function}, Context, kModule, Image.Arch, {},
                             &Image, Image.Format);
  ASSERT_TRUE(Module);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto *Entry = Module->getFunction(kScalar);
  ASSERT_NE(Entry, nullptr);
  EXPECT_EQ(Entry->arg_size(), Hint.Parameters.size());
  EXPECT_TRUE(Module->getModuleFlag(kArm64eAnalysisModuleFlag));
  EXPECT_FALSE(Codegen()
                   .compile(*Module, Image.Arch, Image.Format, kArm64eTriple)
                   .Success);
}

TEST(AArch64EntryABI, IndependentNativeCallersAgreeAtO0AndO2) {
#if defined(__APPLE__) && defined(__aarch64__) && defined(NEVERD_TEST_CLANG)
  llvm::LLVMContext Context;
  auto Module = module(Context);
  addFixtures(*Module);
  ASSERT_FALSE(HasFailure());
  auto Object =
      Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO, kTriple);
  ASSERT_TRUE(Object.Success);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kTemporary, Directory));
  const std::filesystem::path Root(Directory.str().str());
  RecordProperty(kArtifactProperty, Root.string());
  struct Cleanup {
    std::filesystem::path Path;
    ~Cleanup() { std::filesystem::remove_all(Path); }
  } Cleanup{Root};
  const auto ObjectPath = (Root / kObject).string();
  const auto CallerPath = (Root / kCallerFile).string();
  const auto Executable = (Root / kExecutable).string();
  std::ofstream(ObjectPath, std::ios::binary)
      .write(reinterpret_cast<const char *>(Object.ObjectData.data()),
             Object.ObjectData.size());
  std::ofstream(CallerPath) << kNativeCaller;
  for (const char *Optimization : {kOptimize0, kOptimize2}) {
    SCOPED_TRACE(Optimization);
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  NEVERD_TEST_CLANG, {NEVERD_TEST_CLANG, kTargetFlag, kTriple,
                                      Optimization, CallerPath, ObjectPath,
                                      kCxxRuntime, kOutputFlag, Executable}),
              0);
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0);
  }
#else
  GTEST_SKIP() << kUnavailable;
#endif
}
} // namespace
