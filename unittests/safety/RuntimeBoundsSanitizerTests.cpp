//===- RuntimeBoundsSanitizerTests.cpp - LLVM bounds guard tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/pass/ir/safety/RuntimeBoundsSanitizer.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <memory>
#include <string>

namespace {

using namespace neverd;
using namespace neverd::safety;
using namespace neverd::safety_callsite_md;

SafetyCallsiteRecord record(uint64_t CallVA = 0x401020,
                            uint32_t CallSiteId = 4) {
  SafetyCallsiteRecord Record;
  Record.Occurrence = {/*FuncEntry=*/0x401000,
                       /*CallVA=*/CallVA,
                       /*BlockId=*/1,
                       /*OpIdx=*/2,
                       /*OriginSeq=*/3,
                       /*CallSiteId=*/CallSiteId};
  Record.Kind = SemanticKind::Memcpy;
  Record.DestinationOperandIndex = 0;
  Record.LengthOperandIndex = 2;
  Record.ElementBytes = 1;
  return Record;
}

RuntimeSanitizerGuard guard(const SafetyCallsiteRecord &Record,
                            uint64_t RemainingCapacity) {
  RuntimeSanitizerGuard Guard;
  Guard.Occurrence = Record.Occurrence;
  Guard.Kind = Record.Kind;
  Guard.RemainingCapacity = RemainingCapacity;
  Guard.DestinationOperandIndex = Record.DestinationOperandIndex;
  Guard.LengthOperandIndex = Record.LengthOperandIndex;
  Guard.ElementBytes = Record.ElementBytes;
  return Guard;
}

std::unique_ptr<llvm::Module>
makeCallModule(llvm::LLVMContext &Context, llvm::Type *LengthType,
               const SafetyCallsiteRecord &Record, unsigned Copies = 1,
               llvm::Type *DestinationType = nullptr) {
  auto Module = std::make_unique<llvm::Module>("runtime-bounds", Context);
  llvm::Type *PointerType = llvm::PointerType::getUnqual(Context);
  if (!DestinationType)
    DestinationType = PointerType;
  auto *SinkType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {DestinationType, PointerType, LengthType},
                              /*isVarArg=*/false);
  llvm::Function *Sink = llvm::Function::Create(
      SinkType, llvm::GlobalValue::ExternalLinkage, "counted_write", *Module);
  llvm::Function *Caller = llvm::Function::Create(
      SinkType, llvm::GlobalValue::ExternalLinkage, "caller", *Module);
  auto Argument = Caller->arg_begin();
  llvm::Value *Destination = &*Argument++;
  llvm::Value *Source = &*Argument++;
  llvm::Value *Length = &*Argument;
  Destination->setName("destination");
  Source->setName("source");
  Length->setName("length");

  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Context, "entry", Caller);
  llvm::IRBuilder<> Builder(Entry);
  for (unsigned Index = 0; Index < Copies; ++Index) {
    llvm::CallInst *Call =
        Builder.CreateCall(Sink, {Destination, Source, Length});
    if (llvm::Error Error = attach(*Call, Record))
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }
  Builder.CreateRetVoid();
  return Module;
}

std::unique_ptr<llvm::Module>
makeInvokeModule(llvm::LLVMContext &Context,
                 const SafetyCallsiteRecord &Record) {
  auto Module =
      std::make_unique<llvm::Module>("runtime-bounds-invoke", Context);
  llvm::Type *PointerType = llvm::PointerType::getUnqual(Context);
  llvm::Type *LengthType = llvm::Type::getInt64Ty(Context);
  auto *SinkType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(Context), {PointerType, PointerType, LengthType},
      /*isVarArg=*/false);
  llvm::Function *Sink = llvm::Function::Create(
      SinkType, llvm::GlobalValue::ExternalLinkage, "counted_write", *Module);
  llvm::Function *Personality = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), {},
                              /*isVarArg=*/true),
      llvm::GlobalValue::ExternalLinkage, "__gxx_personality_v0", *Module);
  llvm::Function *Caller = llvm::Function::Create(
      SinkType, llvm::GlobalValue::ExternalLinkage, "caller", *Module);
  Caller->setPersonalityFn(Personality);

  auto Argument = Caller->arg_begin();
  llvm::Value *Destination = &*Argument++;
  llvm::Value *Source = &*Argument++;
  llvm::Value *Length = &*Argument;
  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Context, "entry", Caller);
  llvm::BasicBlock *Normal =
      llvm::BasicBlock::Create(Context, "normal", Caller);
  llvm::BasicBlock *Unwind =
      llvm::BasicBlock::Create(Context, "unwind", Caller);

  llvm::IRBuilder<> EntryBuilder(Entry);
  llvm::InvokeInst *Invoke = EntryBuilder.CreateInvoke(
      Sink, Normal, Unwind, {Destination, Source, Length});
  if (llvm::Error Error = attach(*Invoke, Record))
    ADD_FAILURE() << llvm::toString(std::move(Error));
  llvm::IRBuilder<>(Normal).CreateRetVoid();

  llvm::IRBuilder<> UnwindBuilder(Unwind);
  llvm::StructType *LandingPadType =
      llvm::StructType::get(PointerType, llvm::Type::getInt32Ty(Context));
  llvm::LandingPadInst *LandingPad =
      UnwindBuilder.CreateLandingPad(LandingPadType, 0, "exception");
  LandingPad->setCleanup(true);
  UnwindBuilder.CreateResume(LandingPad);
  return Module;
}

std::string render(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  Module.print(OS, nullptr);
  return OS.str();
}

llvm::CallBase *findGuardedCall(llvm::Module &Module) {
  llvm::Function *Caller = Module.getFunction("caller");
  if (!Caller)
    return nullptr;
  for (llvm::Instruction &Instruction : llvm::instructions(*Caller))
    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
      if (Call->getMetadata(Attachment))
        return Call;
  return nullptr;
}

bool blockTraps(const llvm::BasicBlock &Block) {
  for (const llvm::Instruction &Instruction : Block)
    if (const auto *Intrinsic =
            llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction))
      if (Intrinsic->getIntrinsicID() == llvm::Intrinsic::trap)
        return true;
  return false;
}

llvm::CondBrInst *guardBranchFor(llvm::CallBase &Call) {
  llvm::BasicBlock *GuardBlock = Call.getParent()->getSinglePredecessor();
  return GuardBlock
             ? llvm::dyn_cast<llvm::CondBrInst>(GuardBlock->getTerminator())
             : nullptr;
}

llvm::ICmpInst *capacityCompare(llvm::Value *Condition) {
  if (auto *Compare = llvm::dyn_cast<llvm::ICmpInst>(Condition))
    return Compare;
  if (auto *Both = llvm::dyn_cast<llvm::BinaryOperator>(Condition)) {
    if (Both->getOpcode() != llvm::Instruction::And)
      return nullptr;
    if (auto *Compare = llvm::dyn_cast<llvm::ICmpInst>(Both->getOperand(0)))
      return Compare;
    return llvm::dyn_cast<llvm::ICmpInst>(Both->getOperand(1));
  }
  return nullptr;
}

void expectTransactionalFailure(const RuntimeBoundsSanitizerResult &Result,
                                RuntimeBoundsSanitizerError ExpectedError,
                                const std::unique_ptr<llvm::Module> &Module,
                                const llvm::Module *BeforeModule,
                                const std::string &BeforeIR,
                                bool ExpectOccurrence = true) {
  EXPECT_FALSE(Result.Complete);
  EXPECT_EQ(Result.Error, ExpectedError) << Result.Detail;
  EXPECT_EQ(Module.get(), BeforeModule);
  EXPECT_EQ(render(*Module), BeforeIR);
  EXPECT_TRUE(Result.GuardedOriginalEntries.empty());
  EXPECT_EQ(Result.FailureOccurrence.has_value(), ExpectOccurrence);
}

TEST(RuntimeBoundsSanitizerPass,
     ExactBoundaryUsesUnsignedGuardBeforeOriginalCall) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record);
  llvm::Module *OriginalModule = Module.get();

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  ASSERT_TRUE(Result.Complete) << Result.Detail;
  EXPECT_NE(Module.get(), OriginalModule);
  ASSERT_EQ(Result.Error, RuntimeBoundsSanitizerError::None);
  ASSERT_EQ(Result.GuardedOriginalEntries,
            (std::vector<va_t>{Record.Occurrence.FuncEntry}));
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  ASSERT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;

  llvm::CallBase *Call = findGuardedCall(*Module);
  ASSERT_NE(Call, nullptr);
  llvm::BasicBlock *Safe = Call->getParent();
  llvm::CondBrInst *Branch = guardBranchFor(*Call);
  ASSERT_NE(Branch, nullptr);
  EXPECT_EQ(Branch->getSuccessor(0), Safe);
  ASSERT_TRUE(blockTraps(*Branch->getSuccessor(1)));

  auto *Compare = llvm::dyn_cast<llvm::ICmpInst>(Branch->getCondition());
  ASSERT_NE(Compare, nullptr);
  EXPECT_EQ(Compare->getPredicate(), llvm::ICmpInst::ICMP_ULE);
  auto *Capacity = llvm::dyn_cast<llvm::ConstantInt>(Compare->getOperand(1));
  ASSERT_NE(Capacity, nullptr);
  EXPECT_EQ(Capacity->getZExtValue(), 5u);
  EXPECT_TRUE(llvm::ICmpInst::compare(llvm::APInt(64, 5), llvm::APInt(64, 5),
                                      Compare->getPredicate()));
  EXPECT_FALSE(llvm::ICmpInst::compare(llvm::APInt(64, 6), llvm::APInt(64, 5),
                                       Compare->getPredicate()));
}

TEST(RuntimeBoundsSanitizerPass, OnePastEndAllowsOnlyZeroLength) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 0)});

  ASSERT_TRUE(Result.Complete) << Result.Detail;
  llvm::CallBase *Call = findGuardedCall(*Module);
  ASSERT_NE(Call, nullptr);
  llvm::CondBrInst *Branch = guardBranchFor(*Call);
  ASSERT_NE(Branch, nullptr);
  EXPECT_EQ(Branch->getSuccessor(0), Call->getParent());
  EXPECT_TRUE(blockTraps(*Branch->getSuccessor(1)));
  llvm::ICmpInst *Compare = capacityCompare(Branch->getCondition());
  ASSERT_NE(Compare, nullptr);
  EXPECT_EQ(Compare->getPredicate(), llvm::ICmpInst::ICMP_ULE);
  EXPECT_TRUE(llvm::ICmpInst::compare(llvm::APInt(64, 0), llvm::APInt(64, 0),
                                      Compare->getPredicate()));
  EXPECT_FALSE(llvm::ICmpInst::compare(llvm::APInt(64, 1), llvm::APInt(64, 0),
                                       Compare->getPredicate()));
}

TEST(RuntimeBoundsSanitizerPass, ZeroExtendsNarrowLengthsBeforeComparison) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt16Ty(Context), Record);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  ASSERT_TRUE(Result.Complete) << Result.Detail;
  llvm::CallBase *Call = findGuardedCall(*Module);
  ASSERT_NE(Call, nullptr);
  llvm::CondBrInst *Branch = guardBranchFor(*Call);
  ASSERT_NE(Branch, nullptr);
  llvm::ICmpInst *Compare = capacityCompare(Branch->getCondition());
  ASSERT_NE(Compare, nullptr);
  ASSERT_TRUE(Compare->getOperand(0)->getType()->isIntegerTy(64));
  auto *Extend = llvm::dyn_cast<llvm::ZExtInst>(Compare->getOperand(0));
  ASSERT_NE(Extend, nullptr);
  EXPECT_TRUE(Extend->getOperand(0)->getType()->isIntegerTy(16));
}

TEST(RuntimeBoundsSanitizerPass,
     TargetWidthIntegerDestinationIsAcceptedFromTriple) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record,
                     /*Copies=*/1, llvm::Type::getInt64Ty(Context));
  Module->setTargetTriple(llvm::Triple("x86_64-unknown-linux-gnu"));

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  ASSERT_TRUE(Result.Complete) << Result.Detail;
  EXPECT_EQ(Result.Error, RuntimeBoundsSanitizerError::None);
  ASSERT_NE(findGuardedCall(*Module), nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;
}

TEST(RuntimeBoundsSanitizerPass,
     ExplicitDataLayoutDeterminesIntegerDestinationWidth) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt32Ty(Context), Record,
                     /*Copies=*/1, llvm::Type::getInt32Ty(Context));
  Module->setDataLayout("e-p:32:32");

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  ASSERT_TRUE(Result.Complete) << Result.Detail;
  EXPECT_EQ(Result.Error, RuntimeBoundsSanitizerError::None);
  ASSERT_NE(findGuardedCall(*Module), nullptr);
}

TEST(RuntimeBoundsSanitizerPass,
     WideElementCountChecksMultiplicationOverflowAndCapacity) {
  llvm::LLVMContext Context;
  SafetyCallsiteRecord Record = record();
  Record.ElementBytes = 4;
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 20)});

  ASSERT_TRUE(Result.Complete) << Result.Detail;
  llvm::CallBase *Call = findGuardedCall(*Module);
  ASSERT_NE(Call, nullptr);
  llvm::CondBrInst *Branch = guardBranchFor(*Call);
  ASSERT_NE(Branch, nullptr);
  auto *SafeWrite =
      llvm::dyn_cast<llvm::BinaryOperator>(Branch->getCondition());
  ASSERT_NE(SafeWrite, nullptr);
  EXPECT_EQ(SafeWrite->getOpcode(), llvm::Instruction::And);
  llvm::ICmpInst *Compare = capacityCompare(SafeWrite);
  ASSERT_NE(Compare, nullptr);
  EXPECT_EQ(Compare->getPredicate(), llvm::ICmpInst::ICMP_ULE);

  llvm::IntrinsicInst *Multiply = nullptr;
  for (llvm::Instruction &Instruction : *Branch->getParent())
    if (auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction))
      if (Intrinsic->getIntrinsicID() == llvm::Intrinsic::umul_with_overflow)
        Multiply = Intrinsic;
  ASSERT_NE(Multiply, nullptr);
  auto *ElementBytes =
      llvm::dyn_cast<llvm::ConstantInt>(Multiply->getArgOperand(1));
  ASSERT_NE(ElementBytes, nullptr);
  EXPECT_EQ(ElementBytes->getZExtValue(), 4u);
  auto *Bytes = llvm::dyn_cast<llvm::ExtractValueInst>(Compare->getOperand(0));
  ASSERT_NE(Bytes, nullptr);
  ASSERT_EQ(Bytes->getNumIndices(), 1u);
  EXPECT_EQ(*Bytes->idx_begin(), 0u);
  EXPECT_EQ(Bytes->getAggregateOperand(), Multiply);
  auto *Capacity = llvm::dyn_cast<llvm::ConstantInt>(Compare->getOperand(1));
  ASSERT_NE(Capacity, nullptr);
  EXPECT_EQ(Capacity->getZExtValue(), 20u);

  llvm::Value *NoOverflow = SafeWrite->getOperand(0) == Compare
                                ? SafeWrite->getOperand(1)
                                : SafeWrite->getOperand(0);
  auto *Invert = llvm::dyn_cast<llvm::BinaryOperator>(NoOverflow);
  ASSERT_NE(Invert, nullptr);
  ASSERT_EQ(Invert->getOpcode(), llvm::Instruction::Xor);
  llvm::ExtractValueInst *Overflow = nullptr;
  for (llvm::Use &Operand : Invert->operands())
    if (auto *Extract = llvm::dyn_cast<llvm::ExtractValueInst>(Operand.get()))
      Overflow = Extract;
  ASSERT_NE(Overflow, nullptr);
  ASSERT_EQ(Overflow->getNumIndices(), 1u);
  EXPECT_EQ(*Overflow->idx_begin(), 1u);
  EXPECT_EQ(Overflow->getAggregateOperand(), Multiply);

  bool MultiplyOverflow = false;
  EXPECT_EQ(llvm::APInt(64, 5).umul_ov(llvm::APInt(64, 4), MultiplyOverflow),
            llvm::APInt(64, 20));
  EXPECT_FALSE(MultiplyOverflow);
  (void)llvm::APInt::getMaxValue(64).umul_ov(llvm::APInt(64, 4),
                                             MultiplyOverflow);
  EXPECT_TRUE(MultiplyOverflow);
}

TEST(RuntimeBoundsSanitizerPass, InvokeRetainsNormalAndUnwindSuccessors) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module = makeInvokeModule(Context, Record);
  std::string BeforeVerification;
  llvm::raw_string_ostream BeforeVerificationOS(BeforeVerification);
  ASSERT_FALSE(llvm::verifyModule(*Module, &BeforeVerificationOS))
      << BeforeVerification;

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  ASSERT_TRUE(Result.Complete) << Result.Detail;
  auto *Invoke =
      llvm::dyn_cast_or_null<llvm::InvokeInst>(findGuardedCall(*Module));
  ASSERT_NE(Invoke, nullptr);
  EXPECT_EQ(Invoke->getNormalDest()->getName(), "normal");
  EXPECT_EQ(Invoke->getUnwindDest()->getName(), "unwind");
  llvm::CondBrInst *Branch = guardBranchFor(*Invoke);
  ASSERT_NE(Branch, nullptr);
  EXPECT_EQ(Branch->getSuccessor(0), Invoke->getParent());
  EXPECT_TRUE(blockTraps(*Branch->getSuccessor(1)));
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;
}

TEST(RuntimeBoundsSanitizerPass, MissingTargetLeavesOriginalModuleUntouched) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record);
  RuntimeSanitizerGuard Missing = guard(Record, 5);
  ++Missing.Occurrence.CallVA;
  llvm::Module *BeforeModule = Module.get();
  const std::string BeforeIR = render(*Module);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {Missing});

  expectTransactionalFailure(Result, RuntimeBoundsSanitizerError::MissingTarget,
                             Module, BeforeModule, BeforeIR);
}

TEST(RuntimeBoundsSanitizerPass, DuplicateTargetLeavesOriginalModuleUntouched) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record,
                     /*Copies=*/2);
  llvm::Module *BeforeModule = Module.get();
  const std::string BeforeIR = render(*Module);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  expectTransactionalFailure(Result,
                             RuntimeBoundsSanitizerError::DuplicateTarget,
                             Module, BeforeModule, BeforeIR);
}

TEST(RuntimeBoundsSanitizerPass,
     MalformedMetadataLeavesOriginalModuleUntouched) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record);
  llvm::CallBase *Call = findGuardedCall(*Module);
  ASSERT_NE(Call, nullptr);
  Call->setMetadata(Attachment, llvm::MDNode::get(Context, {}));
  llvm::Module *BeforeModule = Module.get();
  const std::string BeforeIR = render(*Module);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  expectTransactionalFailure(Result,
                             RuntimeBoundsSanitizerError::MalformedMetadata,
                             Module, BeforeModule, BeforeIR,
                             /*ExpectOccurrence=*/false);
}

TEST(RuntimeBoundsSanitizerPass,
     MetadataMismatchLeavesOriginalModuleUntouched) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record);
  RuntimeSanitizerGuard Mismatch = guard(Record, 5);
  Mismatch.ElementBytes = 2;
  llvm::Module *BeforeModule = Module.get();
  const std::string BeforeIR = render(*Module);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {Mismatch});

  expectTransactionalFailure(Result,
                             RuntimeBoundsSanitizerError::MetadataMismatch,
                             Module, BeforeModule, BeforeIR);
}

TEST(RuntimeBoundsSanitizerPass, DuplicateGuardLeavesOriginalModuleUntouched) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record);
  const RuntimeSanitizerGuard Guard = guard(Record, 5);
  llvm::Module *BeforeModule = Module.get();
  const std::string BeforeIR = render(*Module);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {Guard, Guard});

  expectTransactionalFailure(Result,
                             RuntimeBoundsSanitizerError::DuplicateGuard,
                             Module, BeforeModule, BeforeIR);
}

TEST(RuntimeBoundsSanitizerPass,
     UnexpectedMetadataTargetLeavesOriginalModuleUntouched) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record);
  llvm::Module *BeforeModule = Module.get();
  const std::string BeforeIR = render(*Module);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {});

  expectTransactionalFailure(Result,
                             RuntimeBoundsSanitizerError::UnexpectedTarget,
                             Module, BeforeModule, BeforeIR);
}

TEST(RuntimeBoundsSanitizerPass,
     LengthTypeMismatchLeavesOriginalModuleUntouched) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::PointerType::getUnqual(Context), Record);
  std::string BeforeVerification;
  llvm::raw_string_ostream BeforeVerificationOS(BeforeVerification);
  ASSERT_FALSE(llvm::verifyModule(*Module, &BeforeVerificationOS))
      << BeforeVerification;
  llvm::Module *BeforeModule = Module.get();
  const std::string BeforeIR = render(*Module);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  expectTransactionalFailure(Result,
                             RuntimeBoundsSanitizerError::OperandTypeMismatch,
                             Module, BeforeModule, BeforeIR);
}

TEST(RuntimeBoundsSanitizerPass,
     WrongWidthIntegerDestinationLeavesOriginalModuleUntouched) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record,
                     /*Copies=*/1, llvm::Type::getInt32Ty(Context));
  Module->setTargetTriple(llvm::Triple("x86_64-unknown-linux-gnu"));
  llvm::Module *BeforeModule = Module.get();
  const std::string BeforeIR = render(*Module);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  expectTransactionalFailure(Result,
                             RuntimeBoundsSanitizerError::OperandTypeMismatch,
                             Module, BeforeModule, BeforeIR);
}

TEST(RuntimeBoundsSanitizerPass,
     VectorDestinationLeavesOriginalModuleUntouched) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  llvm::Type *VectorType =
      llvm::FixedVectorType::get(llvm::Type::getInt32Ty(Context), 2);
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record,
                     /*Copies=*/1, VectorType);
  Module->setTargetTriple(llvm::Triple("x86_64-unknown-linux-gnu"));
  llvm::Module *BeforeModule = Module.get();
  const std::string BeforeIR = render(*Module);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  expectTransactionalFailure(Result,
                             RuntimeBoundsSanitizerError::OperandTypeMismatch,
                             Module, BeforeModule, BeforeIR);
}

TEST(RuntimeBoundsSanitizerPass,
     IntegerDestinationWithoutTargetWidthLeavesOriginalModuleUntouched) {
  llvm::LLVMContext Context;
  const SafetyCallsiteRecord Record = record();
  std::unique_ptr<llvm::Module> Module =
      makeCallModule(Context, llvm::Type::getInt64Ty(Context), Record,
                     /*Copies=*/1, llvm::Type::getInt64Ty(Context));
  llvm::Module *BeforeModule = Module.get();
  const std::string BeforeIR = render(*Module);

  const RuntimeBoundsSanitizerResult Result =
      applyRuntimeBoundsSanitizer(Module, {guard(Record, 5)});

  expectTransactionalFailure(Result,
                             RuntimeBoundsSanitizerError::OperandTypeMismatch,
                             Module, BeforeModule, BeforeIR);
}

} // namespace
