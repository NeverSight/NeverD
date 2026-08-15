//===- X86TranslationBlockLowererTests.cpp - Canonical block IR tests ---===//

#include "gtest/gtest.h"

#include "neverd/lift/X86Regs.h"
#include "neverd/translate/GuestMemoryRuntime.h"
#include "neverd/translate/RuntimeABI.h"
#include "neverd/translate/RuntimeGuestState.h"
#include "neverd/translate/RuntimeHelpers.h"
#include "neverd/translate/TranslationBlockLowerer.h"
#include "neverd/translate/TranslationTargetMachine.h"
#include "neverd/translate/X86TranslationBlockBuilder.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Triple.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

constexpr llvm::StringLiteral HostTriple("aarch64-unknown-linux-gnu");
constexpr uint64_t EntryPC = 0x401000;
static_assert(kX86TranslationBlockLoweringSchemaV1 == 8);

TranslationOptions aarch64AOTOptions() {
  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;
  Options.Mode = TranslationMode::AOT;
  Options.Target.Kind = HostTargetKind::Explicit;
  Options.Target.Architecture = GuestArchitecture::AArch64;
  Options.Target.Triple = HostTriple.str();
  Options.Optimization = TranslationOptimizationPolicy::None;
  Options.LLVMLevel = LLVMOptimizationLevel::O0;
  Options.BlockCache = BlockCachePolicy::Disabled;
  Options.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  return Options;
}

llvm::Expected<TranslationBlockDescriptorV1>
tryBlockFromBytes(std::vector<uint8_t> Bytes) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          17, std::move(Bytes)});
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      llvm::cantFail(GuestMemoryRuntime::create(State));
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());
  return Builder->build(*Runtime, EntryPC);
}

TranslationBlockDescriptorV1 blockFromBytes(std::vector<uint8_t> Bytes) {
  return llvm::cantFail(tryBlockFromBytes(std::move(Bytes)));
}

TranslationBlockDescriptorV1 fixtureBlock() {
  return blockFromBytes({0x48, 0x89, 0xf8, 0x48, 0x83, 0xc0, 0x01, 0xc3});
}

struct AArch64HostContract {
  ResolvedHostTarget Target;
  llvm::DataLayout Layout;
};

const AArch64HostContract &aarch64HostContract() {
  static const AArch64HostContract Contract = [] {
    TranslationTargetMachineV1 Machine =
        llvm::cantFail(createTranslationTargetMachineV1(aarch64AOTOptions()));
    return AArch64HostContract{Machine.hostTarget(), Machine.dataLayout()};
  }();
  return Contract;
}

ResolvedHostTarget resolvedAArch64() { return aarch64HostContract().Target; }

const llvm::DataLayout &aarch64DataLayout() {
  return aarch64HostContract().Layout;
}

void expectValidRuntimeIR(const llvm::Module &Module) {
  const llvm::Triple Triple(Module.getTargetTriple());
  if (llvm::Error Error =
          verifyRuntimeTranslationIRV1(Module, Triple, Module.getDataLayout(),
                                       kRuntimeGuestStateX86_64SizeV1,
                                       runtimeGuestStateX86_64MemorySlotsV1()))
    ADD_FAILURE() << llvm::toString(std::move(Error));

  llvm::SmallVector<TranslationIRMemorySlot, 32> Slots(
      runtimeGuestStateX86_64MemorySlotsV1().begin(),
      runtimeGuestStateX86_64MemorySlotsV1().end());
  Slots.append(runtimeABIMemorySlotsV1().begin(),
               runtimeABIMemorySlotsV1().end());
  const std::vector<TranslationRuntimeHelper> Helpers =
      createRuntimeABIHelperPolicyV1(Module.getContext());
  if (llvm::Error Error =
          verifyTranslationIR(Module, Triple, Module.getDataLayout(),
                              kRuntimeGuestStateX86_64SizeV1,
                              kRuntimeControlBlockSizeV1, Slots, Helpers))
    ADD_FAILURE() << llvm::toString(std::move(Error));
}

std::optional<int64_t> pointerOffset(const llvm::Value *Pointer,
                                     const llvm::Value *ExpectedBase,
                                     const llvm::DataLayout &Layout) {
  int64_t Offset = 0;
  const llvm::Value *Base =
      llvm::GetPointerBaseWithConstantOffset(Pointer, Offset, Layout);
  if (Base != ExpectedBase)
    return std::nullopt;
  return Offset;
}

template <typename Predicate>
llvm::CallInst *findCall(llvm::Function &Function, Predicate Matches) {
  for (llvm::Instruction &Instruction : llvm::instructions(Function))
    if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
      if (Matches(*Call))
        return Call;
  return nullptr;
}

enum class ReferenceArithmetic { Add, Sub };
enum class ReferenceLogic { And, Or, Xor };

struct ArithmeticTruth {
  uint64_t Result = 0;
  uint8_t CF = 0;
  uint8_t PF = 0;
  uint8_t AF = 0;
  uint8_t ZF = 0;
  uint8_t SF = 0;
  uint8_t OF = 0;
};

uint8_t evenParity(uint8_t Value) {
  uint8_t Parity = 1;
  for (unsigned Bit = 0; Bit != 8; ++Bit)
    Parity ^= static_cast<uint8_t>((Value >> Bit) & 1u);
  return Parity;
}

ArithmeticTruth referenceArithmetic(uint64_t Left, uint64_t Right,
                                    ReferenceArithmetic Operation) {
  ArithmeticTruth Truth;
  if (Operation == ReferenceArithmetic::Add) {
    Truth.Result = Left + Right;
    Truth.CF = Truth.Result < Left;
    Truth.OF = static_cast<uint8_t>(
        ((~(Left ^ Right) & (Left ^ Truth.Result)) >> 63) & 1u);
  } else {
    Truth.Result = Left - Right;
    Truth.CF = Left < Right;
    Truth.OF = static_cast<uint8_t>(
        (((Left ^ Right) & (Left ^ Truth.Result)) >> 63) & 1u);
  }
  Truth.PF = evenParity(static_cast<uint8_t>(Truth.Result));
  Truth.AF = static_cast<uint8_t>(((Left ^ Right ^ Truth.Result) >> 4) & 1u);
  Truth.ZF = Truth.Result == 0;
  Truth.SF = static_cast<uint8_t>((Truth.Result >> 63) & 1u);
  return Truth;
}

ArithmeticTruth referenceLogic(uint64_t Left, uint64_t Right,
                               ReferenceLogic Operation) {
  ArithmeticTruth Truth;
  switch (Operation) {
  case ReferenceLogic::And:
    Truth.Result = Left & Right;
    break;
  case ReferenceLogic::Or:
    Truth.Result = Left | Right;
    break;
  case ReferenceLogic::Xor:
    Truth.Result = Left ^ Right;
    break;
  }
  Truth.PF = evenParity(static_cast<uint8_t>(Truth.Result));
  Truth.ZF = Truth.Result == 0;
  Truth.SF = static_cast<uint8_t>((Truth.Result >> 63) & 1u);
  return Truth;
}

std::optional<llvm::APInt> evaluateIntegerIR(const llvm::Value *Value) {
  if (const auto *Integer = llvm::dyn_cast<llvm::ConstantInt>(Value))
    return Integer->getValue();

  if (const auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(Value)) {
    std::optional<llvm::APInt> Left = evaluateIntegerIR(Binary->getOperand(0));
    std::optional<llvm::APInt> Right = evaluateIntegerIR(Binary->getOperand(1));
    if (!Left || !Right || Left->getBitWidth() != Right->getBitWidth())
      return std::nullopt;
    switch (Binary->getOpcode()) {
    case llvm::Instruction::Add:
      return *Left + *Right;
    case llvm::Instruction::Sub:
      return *Left - *Right;
    case llvm::Instruction::And:
      return *Left & *Right;
    case llvm::Instruction::Or:
      return *Left | *Right;
    case llvm::Instruction::Xor:
      return *Left ^ *Right;
    case llvm::Instruction::LShr: {
      const uint64_t Amount = Right->getLimitedValue();
      if (Amount >= Left->getBitWidth())
        return std::nullopt;
      return Left->lshr(static_cast<unsigned>(Amount));
    }
    default:
      return std::nullopt;
    }
  }

  if (const auto *Compare = llvm::dyn_cast<llvm::ICmpInst>(Value)) {
    std::optional<llvm::APInt> Left = evaluateIntegerIR(Compare->getOperand(0));
    std::optional<llvm::APInt> Right =
        evaluateIntegerIR(Compare->getOperand(1));
    if (!Left || !Right || Left->getBitWidth() != Right->getBitWidth())
      return std::nullopt;
    bool Result = false;
    switch (Compare->getPredicate()) {
    case llvm::CmpInst::ICMP_EQ:
      Result = *Left == *Right;
      break;
    case llvm::CmpInst::ICMP_NE:
      Result = *Left != *Right;
      break;
    case llvm::CmpInst::ICMP_ULT:
      Result = Left->ult(*Right);
      break;
    case llvm::CmpInst::ICMP_SLT:
      Result = Left->slt(*Right);
      break;
    default:
      return std::nullopt;
    }
    return llvm::APInt(1, Result);
  }

  if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value)) {
    std::optional<llvm::APInt> Operand = evaluateIntegerIR(Cast->getOperand(0));
    if (!Operand || !Cast->getType()->isIntegerTy())
      return std::nullopt;
    const unsigned Width = Cast->getType()->getIntegerBitWidth();
    switch (Cast->getOpcode()) {
    case llvm::Instruction::ZExt:
      return Operand->zext(Width);
    case llvm::Instruction::Trunc:
      return Operand->trunc(Width);
    default:
      return std::nullopt;
    }
  }

  if (const auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(Value)) {
    if (Intrinsic->getIntrinsicID() != llvm::Intrinsic::ctpop)
      return std::nullopt;
    std::optional<llvm::APInt> Operand =
        evaluateIntegerIR(Intrinsic->getArgOperand(0));
    if (!Operand)
      return std::nullopt;
    return llvm::APInt(Operand->getBitWidth(), Operand->popcount());
  }

  return std::nullopt;
}

const llvm::StoreInst *findStateStoreBefore(const llvm::CallInst &Boundary,
                                            const llvm::Value *State,
                                            const llvm::DataLayout &Layout,
                                            int64_t ExpectedOffset) {
  const llvm::StoreInst *Match = nullptr;
  for (const llvm::Instruction &Instruction : *Boundary.getParent()) {
    if (&Instruction == &Boundary)
      break;
    const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
    if (!Store)
      continue;
    if (pointerOffset(Store->getPointerOperand(), State, Layout) ==
        ExpectedOffset)
      Match = Store;
  }
  return Match;
}

void expectStateIntegerBefore(const llvm::CallInst &Boundary,
                              const llvm::Value *State,
                              const llvm::DataLayout &Layout, int64_t Offset,
                              unsigned Width, uint64_t Expected) {
  const llvm::StoreInst *Store =
      findStateStoreBefore(Boundary, State, Layout, Offset);
  ASSERT_NE(Store, nullptr) << "missing state store at offset " << Offset;
  std::optional<llvm::APInt> Value =
      evaluateIntegerIR(Store->getValueOperand());
  ASSERT_TRUE(Value.has_value())
      << "state store is not an independently evaluable integer expression";
  EXPECT_EQ(Value->getBitWidth(), Width);
  EXPECT_EQ(Value->getZExtValue(), Expected);
}

int64_t gprOffset(RuntimeX86_64GPRV1 Register) {
  return static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, GPR) +
                              static_cast<size_t>(Register) * sizeof(uint64_t));
}

void appendU64LE(std::vector<uint8_t> &Bytes, uint64_t Value) {
  for (unsigned Byte = 0; Byte != sizeof(uint64_t); ++Byte)
    Bytes.push_back(static_cast<uint8_t>(Value >> (Byte * 8)));
}

void appendMovImmediate(std::vector<uint8_t> &Bytes,
                        RuntimeX86_64GPRV1 Register, uint64_t Value) {
  const uint8_t Index = static_cast<uint8_t>(Register);
  Bytes.push_back(Index < 8 ? 0x48 : 0x49);
  Bytes.push_back(static_cast<uint8_t>(0xb8u + (Index & 7u)));
  appendU64LE(Bytes, Value);
}

std::vector<uint8_t>
registerArithmeticBlock(uint64_t Left, uint64_t Right,
                        llvm::ArrayRef<uint8_t> Instruction) {
  std::vector<uint8_t> Bytes;
  appendMovImmediate(Bytes, RuntimeX86_64GPRV1::RAX, Left);
  appendMovImmediate(Bytes, RuntimeX86_64GPRV1::RBX, Right);
  Bytes.insert(Bytes.end(), Instruction.begin(), Instruction.end());
  Bytes.push_back(0xc3);
  return Bytes;
}

std::vector<uint8_t>
immediateArithmeticBlock(uint64_t Left, llvm::ArrayRef<uint8_t> Instruction) {
  std::vector<uint8_t> Bytes;
  appendMovImmediate(Bytes, RuntimeX86_64GPRV1::RAX, Left);
  Bytes.insert(Bytes.end(), Instruction.begin(), Instruction.end());
  Bytes.push_back(0xc3);
  return Bytes;
}

void expectArithmeticTruth(llvm::ArrayRef<uint8_t> Bytes, uint64_t Left,
                           uint64_t Right, ReferenceArithmetic Operation) {
  const std::vector<uint8_t> OwnedBytes(Bytes.begin(), Bytes.end());
  TranslationBlockDescriptorV1 Block = blockFromBytes(OwnedBytes);
  EXPECT_EQ(Block.Bytes, OwnedBytes);

  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  llvm::Function *Function =
      LoweredOrErr->module().getFunction(LoweredOrErr->blockSymbol());
  ASSERT_NE(Function, nullptr);
  llvm::CallInst *ReturnLoad = findCall(*Function, [](llvm::CallInst &Call) {
    const llvm::Function *Callee = Call.getCalledFunction();
    return Callee && Callee->getName() == "nvd_rt_v1_load64_le";
  });
  ASSERT_NE(ReturnLoad, nullptr);
  const llvm::Argument *State = Function->getArg(0);
  const llvm::DataLayout &Layout = LoweredOrErr->module().getDataLayout();
  const ArithmeticTruth Truth = referenceArithmetic(Left, Right, Operation);

  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           gprOffset(RuntimeX86_64GPRV1::RAX), 64,
                           Truth.Result);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, CF), 8,
                           Truth.CF);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, PF), 8,
                           Truth.PF);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, AF), 8,
                           Truth.AF);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, ZF), 8,
                           Truth.ZF);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, SF), 8,
                           Truth.SF);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, OF), 8,
                           Truth.OF);
  expectValidRuntimeIR(LoweredOrErr->module());
}

void expectLogicTruth(
    llvm::ArrayRef<uint8_t> Bytes, uint64_t Left, uint64_t Right,
    ReferenceLogic Operation,
    RuntimeX86_64GPRV1 Destination = RuntimeX86_64GPRV1::RAX) {
  const std::vector<uint8_t> OwnedBytes(Bytes.begin(), Bytes.end());
  TranslationBlockDescriptorV1 Block = blockFromBytes(OwnedBytes);
  EXPECT_EQ(Block.Bytes, OwnedBytes);

  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  llvm::Function *Function =
      LoweredOrErr->module().getFunction(LoweredOrErr->blockSymbol());
  ASSERT_NE(Function, nullptr);
  llvm::CallInst *ReturnLoad = findCall(*Function, [](llvm::CallInst &Call) {
    const llvm::Function *Callee = Call.getCalledFunction();
    return Callee && Callee->getName() == "nvd_rt_v1_load64_le";
  });
  ASSERT_NE(ReturnLoad, nullptr);
  const llvm::Argument *State = Function->getArg(0);
  const llvm::DataLayout &Layout = LoweredOrErr->module().getDataLayout();
  const ArithmeticTruth Truth = referenceLogic(Left, Right, Operation);

  expectStateIntegerBefore(*ReturnLoad, State, Layout, gprOffset(Destination),
                           64, Truth.Result);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, CF), 8, 0);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, PF), 8,
                           Truth.PF);
  EXPECT_EQ(findStateStoreBefore(*ReturnLoad, State, Layout,
                                 offsetof(RuntimeGuestStateX86_64V1, AF)),
            nullptr);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, ZF), 8,
                           Truth.ZF);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, SF), 8,
                           Truth.SF);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, OF), 8, 0);
  expectValidRuntimeIR(LoweredOrErr->module());
}

const llvm::StoreInst *findStateStoreInBlock(const llvm::BasicBlock &Block,
                                             const llvm::Value *State,
                                             const llvm::DataLayout &Layout,
                                             int64_t ExpectedOffset) {
  const llvm::StoreInst *Match = nullptr;
  for (const llvm::Instruction &Instruction : Block) {
    const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
    if (!Store)
      continue;
    if (pointerOffset(Store->getPointerOperand(), State, Layout) ==
        ExpectedOffset)
      Match = Store;
  }
  return Match;
}

llvm::BasicBlock *findBlock(llvm::Function &Function, llvm::StringRef Name) {
  for (llvm::BasicBlock &Block : Function)
    if (Block.getName() == Name)
      return &Block;
  return nullptr;
}

void expectReturnTruth(uint64_t InitialRSP, uint16_t Immediate) {
  std::vector<uint8_t> Bytes;
  appendMovImmediate(Bytes, RuntimeX86_64GPRV1::RSP, InitialRSP);
  const uint64_t ReturnPC = EntryPC + Bytes.size();
  if (Immediate == 0) {
    Bytes.push_back(0xc3);
  } else {
    Bytes.push_back(0xc2);
    Bytes.push_back(static_cast<uint8_t>(Immediate));
    Bytes.push_back(static_cast<uint8_t>(Immediate >> 8));
  }

  TranslationBlockDescriptorV1 Block = blockFromBytes(Bytes);
  EXPECT_EQ(Block.Bytes, Bytes);
  EXPECT_EQ(Block.Header.ReturnImmediate, Immediate);
  EXPECT_EQ(hasTranslationBlockDescriptorFlag(
                Block.Header.Flags,
                TranslationBlockDescriptorFlagV1::HasReturnImmediate),
            Immediate != 0);

  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  llvm::Function *Function =
      LoweredOrErr->module().getFunction(LoweredOrErr->blockSymbol());
  ASSERT_NE(Function, nullptr);
  llvm::Argument *State = Function->getArg(0);
  llvm::Argument *Runtime = Function->getArg(1);
  const llvm::DataLayout &Layout = LoweredOrErr->module().getDataLayout();
  llvm::CallInst *ReturnLoad = findCall(*Function, [](llvm::CallInst &Call) {
    const llvm::Function *Callee = Call.getCalledFunction();
    return Callee && Callee->getName() == "nvd_rt_v1_load64_le";
  });
  ASSERT_NE(ReturnLoad, nullptr);
  ASSERT_EQ(ReturnLoad->arg_size(), 3u);

  std::optional<llvm::APInt> LoadAddress =
      evaluateIntegerIR(ReturnLoad->getArgOperand(1));
  ASSERT_TRUE(LoadAddress.has_value());
  EXPECT_EQ(LoadAddress->getBitWidth(), 64u);
  EXPECT_EQ(LoadAddress->getZExtValue(), InitialRSP);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           gprOffset(RuntimeX86_64GPRV1::RSP), 64, InitialRSP);
  expectStateIntegerBefore(*ReturnLoad, State, Layout,
                           offsetof(RuntimeGuestStateX86_64V1, RIP), 64,
                           ReturnPC);

  llvm::BasicBlock *Failure = findBlock(*Function, "return.fault");
  ASSERT_NE(Failure, nullptr);
  EXPECT_FALSE(llvm::any_of(*Failure, [](const llvm::Instruction &Instruction) {
    return llvm::isa<llvm::StoreInst>(&Instruction);
  }));
  const auto *FailureReturn =
      llvm::dyn_cast<llvm::ReturnInst>(Failure->getTerminator());
  ASSERT_NE(FailureReturn, nullptr);
  EXPECT_EQ(FailureReturn->getReturnValue(), ReturnLoad);

  llvm::BasicBlock *Success = findBlock(*Function, "return.loaded");
  ASSERT_NE(Success, nullptr);
  const llvm::StoreInst *RSPStore = findStateStoreInBlock(
      *Success, State, Layout, gprOffset(RuntimeX86_64GPRV1::RSP));
  ASSERT_NE(RSPStore, nullptr);
  std::optional<llvm::APInt> NextRSP =
      evaluateIntegerIR(RSPStore->getValueOperand());
  ASSERT_TRUE(NextRSP.has_value());
  EXPECT_EQ(NextRSP->getBitWidth(), 64u);
  EXPECT_EQ(NextRSP->getZExtValue(), InitialRSP + sizeof(uint64_t) + Immediate);

  const llvm::StoreInst *RIPStore = findStateStoreInBlock(
      *Success, State, Layout, offsetof(RuntimeGuestStateX86_64V1, RIP));
  ASSERT_NE(RIPStore, nullptr);
  const auto *ReturnTarget =
      llvm::dyn_cast<llvm::LoadInst>(RIPStore->getValueOperand());
  ASSERT_NE(ReturnTarget, nullptr);
  EXPECT_EQ(
      pointerOffset(ReturnTarget->getPointerOperand(), Runtime, Layout),
      static_cast<int64_t>(offsetof(RuntimeControlBlockV1, ScalarResult)));

  const auto *SuccessReturn =
      llvm::dyn_cast<llvm::ReturnInst>(Success->getTerminator());
  ASSERT_NE(SuccessReturn, nullptr);
  const auto *ExitKind =
      llvm::dyn_cast<llvm::ConstantInt>(SuccessReturn->getReturnValue());
  ASSERT_NE(ExitKind, nullptr);
  EXPECT_EQ(ExitKind->getZExtValue(),
            static_cast<uint32_t>(BlockExitKindV1::Return));
  expectValidRuntimeIR(LoweredOrErr->module());
}

void expectLoweringError(
    llvm::Expected<LoweredTranslationBlockV1> Result,
    TranslationBlockLoweringErrorCode ExpectedCode,
    std::optional<NdOp> ExpectedOpcode = std::nullopt,
    std::optional<uint64_t> ExpectedGuestPC = std::nullopt) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      Result.takeError(), [&](const TranslationBlockLoweringError &Error) {
        SawTypedError = true;
        EXPECT_EQ(Error.code(), ExpectedCode);
        if (ExpectedOpcode)
          EXPECT_EQ(Error.opcode(), ExpectedOpcode);
        if (ExpectedGuestPC)
          EXPECT_EQ(Error.guestPC(), *ExpectedGuestPC);
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

TEST(X86TranslationBlockLowerer, LowersFixtureToVerifiedCanonicalAArch64IR) {
  llvm::LLVMContext Context;
  const llvm::DataLayout &Layout = aarch64DataLayout();
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(fixtureBlock(), resolvedAArch64(), Layout,
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  LoweredTranslationBlockV1 &Lowered = *LoweredOrErr;
  llvm::Module &Module = Lowered.module();

  EXPECT_EQ(Module.getTargetTriple().normalize(),
            llvm::Triple(HostTriple).normalize());
  EXPECT_EQ(Module.getDataLayout(), Layout);
  llvm::Function *Block = Module.getFunction(Lowered.blockSymbol());
  ASSERT_NE(Block, nullptr);
  EXPECT_FALSE(Block->isDeclaration());
  EXPECT_TRUE(Block->hasHiddenVisibility());
  EXPECT_TRUE(Block->isDSOLocal());
  EXPECT_TRUE(Block->doesNotThrow());
  EXPECT_EQ(Block->getCallingConv(), llvm::CallingConv::C);
  ASSERT_EQ(Block->arg_size(), 2u);

  size_t DefinitionCount = 0;
  for (const llvm::Function &Function : Module)
    DefinitionCount += !Function.isDeclaration();
  EXPECT_EQ(DefinitionCount, 1u);
  EXPECT_NE(Module.getFunction("llvm.ctpop.i8"), nullptr);
  expectValidRuntimeIR(Module);

  for (const llvm::Instruction &Instruction : llvm::instructions(*Block))
    EXPECT_FALSE(llvm::isa<llvm::IntToPtrInst>(&Instruction));
}

TEST(X86TranslationBlockLowerer,
     LowersCanonicalRel8DirectBranchAndCommitsDirtyState) {
  std::vector<uint8_t> Bytes;
  appendMovImmediate(Bytes, RuntimeX86_64GPRV1::RAX,
                     std::numeric_limits<uint64_t>::max());
  appendMovImmediate(Bytes, RuntimeX86_64GPRV1::RBX, 1);
  Bytes.insert(Bytes.end(), {0x48, 0x01, 0xd8}); // add rax, rbx
  const uint64_t BranchPC = EntryPC + Bytes.size();
  Bytes.insert(Bytes.end(), {0xeb, 0x05});
  const uint64_t TargetPC = BranchPC + 2 + 5;

  TranslationBlockDescriptorV1 Block = blockFromBytes(Bytes);
  ASSERT_EQ(Block.Header.Terminator,
            TranslationBlockTerminatorKindV1::DirectBranch);
  ASSERT_TRUE(hasTranslationBlockDescriptorFlag(
      Block.Header.Flags, TranslationBlockDescriptorFlagV1::HasStaticTarget));
  EXPECT_EQ(Block.Header.StaticTargetPC, TargetPC);
  ASSERT_FALSE(Block.Ops.empty());
  EXPECT_EQ(Block.Ops.back().Opcode, NdOp::BRANCH);
  ASSERT_EQ(Block.Ops.back().NumInputs, 1u);
  EXPECT_EQ(Block.Ops.back().Inputs[0], NdVar::cst(TargetPC, 8));

  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  llvm::Function *Function =
      LoweredOrErr->module().getFunction(LoweredOrErr->blockSymbol());
  ASSERT_NE(Function, nullptr);
  ASSERT_EQ(Function->size(), 1u);
  EXPECT_EQ(LoweredOrErr->module().getFunction("nvd_rt_v1_load64_le"), nullptr);

  llvm::BasicBlock &Entry = Function->getEntryBlock();
  const llvm::Argument *State = Function->getArg(0);
  const llvm::DataLayout &Layout = LoweredOrErr->module().getDataLayout();
  std::set<int64_t> CommittedOffsets;
  for (const llvm::Instruction &Instruction : Entry)
    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
      if (std::optional<int64_t> Offset =
              pointerOffset(Store->getPointerOperand(), State, Layout))
        CommittedOffsets.insert(*Offset);
  EXPECT_EQ(
      CommittedOffsets,
      (std::set<int64_t>{
          gprOffset(RuntimeX86_64GPRV1::RAX),
          gprOffset(RuntimeX86_64GPRV1::RBX),
          static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, RIP)),
          static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, CF)),
          static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, PF)),
          static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, AF)),
          static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, ZF)),
          static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, SF)),
          static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, OF))}));

  const llvm::StoreInst *RIPStore = findStateStoreInBlock(
      Entry, State, Layout, offsetof(RuntimeGuestStateX86_64V1, RIP));
  ASSERT_NE(RIPStore, nullptr);
  const auto *StoredTarget =
      llvm::dyn_cast<llvm::ConstantInt>(RIPStore->getValueOperand());
  ASSERT_NE(StoredTarget, nullptr);
  EXPECT_EQ(StoredTarget->getZExtValue(), TargetPC);

  const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(Entry.getTerminator());
  ASSERT_NE(Return, nullptr);
  const auto *ExitKind =
      llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
  ASSERT_NE(ExitKind, nullptr);
  EXPECT_EQ(ExitKind->getZExtValue(),
            static_cast<uint32_t>(BlockExitKindV1::DirectBranch));
  expectValidRuntimeIR(LoweredOrErr->module());
}

TEST(X86TranslationBlockLowerer,
     LowersCanonicalZeroFlagBranchesToASelectedGuestPC) {
  struct BranchCase {
    llvm::ArrayRef<uint8_t> Bytes;
    NdOp PredicateOpcode;
  };
  constexpr std::array<uint8_t, 2> ShortEqual = {0x74, 0x05};
  constexpr std::array<uint8_t, 2> ShortNotEqual = {0x75, 0x05};
  constexpr std::array<uint8_t, 6> NearEqual = {0x0f, 0x84, 0x05,
                                                0x00, 0x00, 0x00};
  constexpr std::array<uint8_t, 6> NearNotEqual = {0x0f, 0x85, 0x05,
                                                   0x00, 0x00, 0x00};
  const std::array<BranchCase, 4> Cases = {{{ShortEqual, NdOp::COPY},
                                            {ShortNotEqual, NdOp::BOOL_NOT},
                                            {NearEqual, NdOp::COPY},
                                            {NearNotEqual, NdOp::BOOL_NOT}}};

  for (const BranchCase &Case : Cases) {
    SCOPED_TRACE(Case.Bytes.size());
    TranslationBlockDescriptorV1 Block = blockFromBytes(
        std::vector<uint8_t>(Case.Bytes.begin(), Case.Bytes.end()));
    ASSERT_EQ(Block.Header.Terminator,
              TranslationBlockTerminatorKindV1::ConditionalBranch);
    ASSERT_TRUE(hasTranslationBlockDescriptorFlag(
        Block.Header.Flags, TranslationBlockDescriptorFlagV1::HasStaticTarget));
    ASSERT_EQ(Block.Ops.size(), 2u);
    EXPECT_EQ(Block.Ops[0].Opcode, Case.PredicateOpcode);
    EXPECT_EQ(Block.Ops[1].Opcode, NdOp::COND_BR);

    llvm::LLVMContext Context;
    llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
        lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                   aarch64DataLayout(), Context);
    ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
        << llvm::toString(LoweredOrErr.takeError());
    llvm::Function *Function =
        LoweredOrErr->module().getFunction(LoweredOrErr->blockSymbol());
    ASSERT_NE(Function, nullptr);
    ASSERT_EQ(Function->size(), 1u);

    const llvm::StoreInst *RIPStore =
        findStateStoreInBlock(Function->getEntryBlock(), Function->getArg(0),
                              LoweredOrErr->module().getDataLayout(),
                              offsetof(RuntimeGuestStateX86_64V1, RIP));
    ASSERT_NE(RIPStore, nullptr);
    const auto *SelectedPC =
        llvm::dyn_cast<llvm::SelectInst>(RIPStore->getValueOperand());
    ASSERT_NE(SelectedPC, nullptr);
    const auto *TakenPC =
        llvm::dyn_cast<llvm::ConstantInt>(SelectedPC->getTrueValue());
    const auto *FallthroughPC =
        llvm::dyn_cast<llvm::ConstantInt>(SelectedPC->getFalseValue());
    ASSERT_NE(TakenPC, nullptr);
    ASSERT_NE(FallthroughPC, nullptr);
    EXPECT_EQ(TakenPC->getZExtValue(), Block.Header.StaticTargetPC);
    EXPECT_EQ(FallthroughPC->getZExtValue(), Block.Header.FallthroughPC);

    const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(
        Function->getEntryBlock().getTerminator());
    ASSERT_NE(Return, nullptr);
    const auto *ExitKind =
        llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
    ASSERT_NE(ExitKind, nullptr);
    EXPECT_EQ(ExitKind->getZExtValue(),
              static_cast<uint32_t>(BlockExitKindV1::DirectBranch));
    expectValidRuntimeIR(LoweredOrErr->module());
  }
}

TEST(X86TranslationBlockLowerer,
     LowersCanonicalSingleFlagBranchesToASelectedGuestPC) {
  struct BranchCase {
    const char *Name;
    std::vector<uint8_t> Bytes;
    NdOp PredicateOpcode;
    uint64_t Flag;
  };
  const std::array<BranchCase, 16> Cases = {{
      {"short-jo", {0x70, 0x05}, NdOp::COPY, x86reg::OF},
      {"near-jo", {0x0f, 0x80, 0x05, 0x00, 0x00, 0x00}, NdOp::COPY, x86reg::OF},
      {"short-jno", {0x71, 0x05}, NdOp::BOOL_NOT, x86reg::OF},
      {"near-jno",
       {0x0f, 0x81, 0x05, 0x00, 0x00, 0x00},
       NdOp::BOOL_NOT,
       x86reg::OF},
      {"short-jb", {0x72, 0x05}, NdOp::COPY, x86reg::CF},
      {"near-jb", {0x0f, 0x82, 0x05, 0x00, 0x00, 0x00}, NdOp::COPY, x86reg::CF},
      {"short-jae", {0x73, 0x05}, NdOp::BOOL_NOT, x86reg::CF},
      {"near-jae",
       {0x0f, 0x83, 0x05, 0x00, 0x00, 0x00},
       NdOp::BOOL_NOT,
       x86reg::CF},
      {"short-js", {0x78, 0x05}, NdOp::COPY, x86reg::SF},
      {"near-js", {0x0f, 0x88, 0x05, 0x00, 0x00, 0x00}, NdOp::COPY, x86reg::SF},
      {"short-jns", {0x79, 0x05}, NdOp::BOOL_NOT, x86reg::SF},
      {"near-jns",
       {0x0f, 0x89, 0x05, 0x00, 0x00, 0x00},
       NdOp::BOOL_NOT,
       x86reg::SF},
      {"short-jp", {0x7a, 0x05}, NdOp::COPY, x86reg::PF},
      {"near-jp", {0x0f, 0x8a, 0x05, 0x00, 0x00, 0x00}, NdOp::COPY, x86reg::PF},
      {"short-jnp", {0x7b, 0x05}, NdOp::BOOL_NOT, x86reg::PF},
      {"near-jnp",
       {0x0f, 0x8b, 0x05, 0x00, 0x00, 0x00},
       NdOp::BOOL_NOT,
       x86reg::PF},
  }};

  for (const BranchCase &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    TranslationBlockDescriptorV1 Block = blockFromBytes(Case.Bytes);
    ASSERT_EQ(Block.Header.Terminator,
              TranslationBlockTerminatorKindV1::ConditionalBranch);
    ASSERT_EQ(Block.Ops.size(), 2u);
    EXPECT_EQ(Block.Ops[0].Opcode, Case.PredicateOpcode);
    EXPECT_EQ(Block.Ops[0].Inputs[0], NdVar::reg(Case.Flag, 1));
    EXPECT_EQ(Block.Ops[1].Opcode, NdOp::COND_BR);

    llvm::LLVMContext Context;
    llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
        lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                   aarch64DataLayout(), Context);
    ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
        << llvm::toString(LoweredOrErr.takeError());
    expectValidRuntimeIR(LoweredOrErr->module());
  }
}

TEST(X86TranslationBlockLowerer, LowersCanonicalShortJBEFromCarryOrZero) {
  TranslationBlockDescriptorV1 Block = blockFromBytes({0x76, 0x05});
  ASSERT_EQ(Block.Header.Terminator,
            TranslationBlockTerminatorKindV1::ConditionalBranch);
  ASSERT_EQ(Block.Ops.size(), 2u);
  EXPECT_EQ(Block.Ops[0].Opcode, NdOp::BOOL_OR);
  EXPECT_EQ(Block.Ops[0].Inputs[0], NdVar::reg(x86reg::CF, 1));
  EXPECT_EQ(Block.Ops[0].Inputs[1], NdVar::reg(x86reg::ZF, 1));
  EXPECT_EQ(Block.Ops[1].Opcode, NdOp::COND_BR);
  EXPECT_EQ(Block.Ops[1].Inputs[1], Block.Ops[0].Output);

  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  expectValidRuntimeIR(LoweredOrErr->module());
}

TEST(X86TranslationBlockLowerer, LowersCanonicalShortJAFromClearCarryAndZero) {
  TranslationBlockDescriptorV1 Block = blockFromBytes({0x77, 0x05});
  ASSERT_EQ(Block.Header.Terminator,
            TranslationBlockTerminatorKindV1::ConditionalBranch);
  ASSERT_EQ(Block.Ops.size(), 4u);
  EXPECT_EQ(Block.Ops[0].Opcode, NdOp::BOOL_NOT);
  EXPECT_EQ(Block.Ops[0].Inputs[0], NdVar::reg(x86reg::CF, 1));
  EXPECT_EQ(Block.Ops[1].Opcode, NdOp::BOOL_NOT);
  EXPECT_EQ(Block.Ops[1].Inputs[0], NdVar::reg(x86reg::ZF, 1));
  EXPECT_EQ(Block.Ops[2].Opcode, NdOp::BOOL_AND);
  EXPECT_EQ(Block.Ops[2].Inputs[0], Block.Ops[0].Output);
  EXPECT_EQ(Block.Ops[2].Inputs[1], Block.Ops[1].Output);
  EXPECT_EQ(Block.Ops[3].Opcode, NdOp::COND_BR);
  EXPECT_EQ(Block.Ops[3].Inputs[1], Block.Ops[2].Output);

  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  expectValidRuntimeIR(LoweredOrErr->module());
}

TEST(X86TranslationBlockLowerer,
     LowersEveryCanonicalUnsignedMultiFlagBranchEncoding) {
  const std::array<std::vector<uint8_t>, 4> Encodings = {{
      {0x76, 0x05},
      {0x0f, 0x86, 0x05, 0x00, 0x00, 0x00},
      {0x77, 0x05},
      {0x0f, 0x87, 0x05, 0x00, 0x00, 0x00},
  }};

  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    TranslationBlockDescriptorV1 Block = blockFromBytes(Bytes);
    llvm::LLVMContext Context;
    llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
        lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                   aarch64DataLayout(), Context);
    ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
        << llvm::toString(LoweredOrErr.takeError());
    expectValidRuntimeIR(LoweredOrErr->module());
  }
}

TEST(X86TranslationBlockLowerer,
     LowersCanonicalSignedMultiFlagBranchesToASelectedGuestPC) {
  struct BranchCase {
    const char *Name;
    std::vector<uint8_t> Bytes;
    std::vector<NdOp> PredicateOpcodes;
  };
  const std::array<BranchCase, 8> Cases = {{
      {"short-jl", {0x7c, 0x05}, {NdOp::INT_NOTEQUAL}},
      {"near-jl", {0x0f, 0x8c, 0x05, 0x00, 0x00, 0x00}, {NdOp::INT_NOTEQUAL}},
      {"short-jge", {0x7d, 0x05}, {NdOp::INT_EQUAL}},
      {"near-jge", {0x0f, 0x8d, 0x05, 0x00, 0x00, 0x00}, {NdOp::INT_EQUAL}},
      {"short-jle", {0x7e, 0x05}, {NdOp::INT_NOTEQUAL, NdOp::BOOL_OR}},
      {"near-jle",
       {0x0f, 0x8e, 0x05, 0x00, 0x00, 0x00},
       {NdOp::INT_NOTEQUAL, NdOp::BOOL_OR}},
      {"short-jg",
       {0x7f, 0x05},
       {NdOp::BOOL_NOT, NdOp::INT_EQUAL, NdOp::BOOL_AND}},
      {"near-jg",
       {0x0f, 0x8f, 0x05, 0x00, 0x00, 0x00},
       {NdOp::BOOL_NOT, NdOp::INT_EQUAL, NdOp::BOOL_AND}},
  }};

  for (const BranchCase &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    TranslationBlockDescriptorV1 Block = blockFromBytes(Case.Bytes);
    ASSERT_EQ(Block.Header.Terminator,
              TranslationBlockTerminatorKindV1::ConditionalBranch);
    ASSERT_EQ(Block.Ops.size(), Case.PredicateOpcodes.size() + 1);
    for (size_t Index = 0; Index != Case.PredicateOpcodes.size(); ++Index)
      EXPECT_EQ(Block.Ops[Index].Opcode, Case.PredicateOpcodes[Index]);
    EXPECT_EQ(Block.Ops.back().Opcode, NdOp::COND_BR);
    EXPECT_EQ(Block.Ops.back().Inputs[1],
              Block.Ops[Case.PredicateOpcodes.size() - 1].Output);

    llvm::LLVMContext Context;
    llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
        lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                   aarch64DataLayout(), Context);
    ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
        << llvm::toString(LoweredOrErr.takeError());
    expectValidRuntimeIR(LoweredOrErr->module());
  }
}

TEST(X86TranslationBlockLowerer,
     AcceptsAZeroDisplacementConditionalBranchAsOneSuccessor) {
  TranslationBlockDescriptorV1 Block = blockFromBytes({0x74, 0x00});
  ASSERT_EQ(Block.Header.Terminator,
            TranslationBlockTerminatorKindV1::ConditionalBranch);
  ASSERT_EQ(Block.Header.StaticTargetPC, Block.Header.FallthroughPC);
  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  expectValidRuntimeIR(LoweredOrErr->module());
}

TEST(X86TranslationBlockLowerer,
     RejectsAConditionalPredicateThatDisagreesWithGuestBytes) {
  TranslationBlockDescriptorV1 Block = blockFromBytes({0x74, 0xfe});
  ASSERT_EQ(Block.Ops.front().Opcode, NdOp::COPY);
  Block.Ops.front().Opcode = NdOp::BOOL_NOT;
  llvm::LLVMContext Context;
  expectLoweringError(lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                                 aarch64DataLayout(), Context),
                      TranslationBlockLoweringErrorCode::InvalidDescriptor);
}

TEST(X86TranslationBlockLowerer,
     RejectsASingleFlagPredicateThatDisagreesWithGuestBytes) {
  TranslationBlockDescriptorV1 Block = blockFromBytes({0x72, 0xfe});
  ASSERT_EQ(Block.Ops.front().Opcode, NdOp::COPY);
  ASSERT_EQ(Block.Ops.front().Inputs[0], NdVar::reg(x86reg::CF, 1));
  Block.Ops.front().Inputs[0] = NdVar::reg(x86reg::ZF, 1);
  llvm::LLVMContext Context;
  expectLoweringError(lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                                 aarch64DataLayout(), Context),
                      TranslationBlockLoweringErrorCode::InvalidDescriptor);
}

TEST(X86TranslationBlockLowerer,
     RejectsMultiFlagPredicatesThatDisagreeWithGuestBytes) {
  TranslationBlockDescriptorV1 Above = blockFromBytes({0x77, 0xfe});
  ASSERT_EQ(Above.Ops.size(), 4u);
  ASSERT_EQ(Above.Ops[2].Opcode, NdOp::BOOL_AND);
  Above.Ops[2].Opcode = NdOp::BOOL_OR;

  TranslationBlockDescriptorV1 LessOrEqual =
      blockFromBytes({0x0f, 0x8e, 0xfa, 0xff, 0xff, 0xff});
  ASSERT_EQ(LessOrEqual.Ops.size(), 3u);
  ASSERT_EQ(LessOrEqual.Ops[0].Opcode, NdOp::INT_NOTEQUAL);
  LessOrEqual.Ops[0].Opcode = NdOp::INT_EQUAL;

  TranslationBlockDescriptorV1 AboveOrder = blockFromBytes({0x77, 0xfe});
  ASSERT_EQ(AboveOrder.Ops.size(), 4u);
  std::swap(AboveOrder.Ops[0], AboveOrder.Ops[1]);

  TranslationBlockDescriptorV1 LessOrEqualInputs = blockFromBytes({0x7e, 0xfe});
  ASSERT_EQ(LessOrEqualInputs.Ops.size(), 3u);
  ASSERT_EQ(LessOrEqualInputs.Ops[1].Opcode, NdOp::BOOL_OR);
  std::swap(LessOrEqualInputs.Ops[1].Inputs[0],
            LessOrEqualInputs.Ops[1].Inputs[1]);

  for (TranslationBlockDescriptorV1 *Block :
       {&Above, &LessOrEqual, &AboveOrder, &LessOrEqualInputs}) {
    llvm::LLVMContext Context;
    expectLoweringError(lowerX86TranslationBlockV1(*Block, resolvedAArch64(),
                                                   aarch64DataLayout(),
                                                   Context),
                        TranslationBlockLoweringErrorCode::InvalidDescriptor);
  }
}

TEST(X86TranslationBlockLowerer,
     CommitsPriorStateAndUsesTheCheckedReturnLoadProtocol) {
  llvm::LLVMContext Context;
  const llvm::DataLayout &Layout = aarch64DataLayout();
  LoweredTranslationBlockV1 Lowered = llvm::cantFail(lowerX86TranslationBlockV1(
      fixtureBlock(), resolvedAArch64(), Layout, Context));
  llvm::Function *Block = Lowered.module().getFunction(Lowered.blockSymbol());
  ASSERT_NE(Block, nullptr);
  llvm::Argument *State = Block->getArg(0);
  llvm::Argument *Runtime = Block->getArg(1);

  llvm::CallInst *Load = findCall(*Block, [](const llvm::CallInst &Call) {
    const llvm::Function *Callee = Call.getCalledFunction();
    return Callee && Callee->getName() == "nvd_rt_v1_load64_le";
  });
  ASSERT_NE(Load, nullptr);
  ASSERT_EQ(Load->arg_size(), 3u);
  EXPECT_EQ(Load->getArgOperand(0), Runtime);
  const auto *RSP = llvm::dyn_cast<llvm::LoadInst>(Load->getArgOperand(1));
  ASSERT_NE(RSP, nullptr);
  EXPECT_EQ(pointerOffset(RSP->getPointerOperand(), State, Layout),
            static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, GPR) +
                                 static_cast<size_t>(RuntimeX86_64GPRV1::RSP) *
                                     sizeof(uint64_t)));
  EXPECT_EQ(RSP->getAlign(), llvm::Align(alignof(uint64_t)));
  const auto *Alignment =
      llvm::dyn_cast<llvm::ConstantInt>(Load->getArgOperand(2));
  ASSERT_NE(Alignment, nullptr);
  EXPECT_EQ(Alignment->getZExtValue(), 1u);

  std::set<int64_t> CommittedOffsets;
  const llvm::StoreInst *FaultRIPStore = nullptr;
  for (llvm::Instruction &Instruction : *Load->getParent()) {
    if (&Instruction == Load)
      break;
    const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
    if (!Store)
      continue;
    if (std::optional<int64_t> Offset =
            pointerOffset(Store->getPointerOperand(), State, Layout)) {
      CommittedOffsets.insert(*Offset);
      if (*Offset ==
          static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, RIP)))
        FaultRIPStore = Store;
    }
  }
  const std::set<int64_t> ExpectedOffsets = {
      static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, GPR)),
      static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, RIP)),
      static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, CF)),
      static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, PF)),
      static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, AF)),
      static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, ZF)),
      static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, SF)),
      static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, OF)),
  };
  EXPECT_EQ(CommittedOffsets, ExpectedOffsets);
  ASSERT_NE(FaultRIPStore, nullptr);
  const auto *FaultPC =
      llvm::dyn_cast<llvm::ConstantInt>(FaultRIPStore->getValueOperand());
  ASSERT_NE(FaultPC, nullptr);
  EXPECT_EQ(FaultPC->getZExtValue(), EntryPC + 7);

  auto *Compare = llvm::dyn_cast_or_null<llvm::ICmpInst>(Load->getNextNode());
  ASSERT_NE(Compare, nullptr);
  auto *Branch =
      llvm::dyn_cast_or_null<llvm::CondBrInst>(Compare->getNextNode());
  ASSERT_NE(Branch, nullptr);
  llvm::BasicBlock *Success = Branch->getSuccessor(0);
  llvm::BasicBlock *Failure = Branch->getSuccessor(1);

  ASSERT_EQ(Failure->size(), 1u);
  auto *FailureReturn = llvm::dyn_cast<llvm::ReturnInst>(&Failure->front());
  ASSERT_NE(FailureReturn, nullptr);
  EXPECT_EQ(FailureReturn->getReturnValue(), Load);

  llvm::LoadInst *TargetLoad = nullptr;
  for (llvm::Instruction &Instruction : *Success) {
    if (llvm::isa<llvm::GetElementPtrInst>(&Instruction))
      continue;
    TargetLoad = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
    break;
  }
  ASSERT_NE(TargetLoad, nullptr);
  EXPECT_EQ(
      pointerOffset(TargetLoad->getPointerOperand(), Runtime, Layout),
      static_cast<int64_t>(offsetof(RuntimeControlBlockV1, ScalarResult)));
  EXPECT_EQ(TargetLoad->getAlign(), llvm::Align(alignof(uint64_t)));

  std::set<int64_t> SuccessStateOffsets;
  for (llvm::Instruction &Instruction : *Success)
    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
      if (std::optional<int64_t> Offset =
              pointerOffset(Store->getPointerOperand(), State, Layout))
        SuccessStateOffsets.insert(*Offset);
  EXPECT_EQ(
      SuccessStateOffsets,
      (std::set<int64_t>{
          static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, GPR) +
                               static_cast<size_t>(RuntimeX86_64GPRV1::RSP) *
                                   sizeof(uint64_t)),
          static_cast<int64_t>(offsetof(RuntimeGuestStateX86_64V1, RIP))}));

  const auto *SuccessReturn =
      llvm::dyn_cast<llvm::ReturnInst>(Success->getTerminator());
  ASSERT_NE(SuccessReturn, nullptr);
  const auto *ExitKind =
      llvm::dyn_cast<llvm::ConstantInt>(SuccessReturn->getReturnValue());
  ASSERT_NE(ExitKind, nullptr);
  EXPECT_EQ(ExitKind->getZExtValue(),
            static_cast<uint32_t>(BlockExitKindV1::Return));
  expectValidRuntimeIR(Lowered.module());
}

TEST(X86TranslationBlockLowerer,
     LowersDifferentFullWidthRegistersAndAddImmediate) {
  TranslationBlockDescriptorV1 Block =
      blockFromBytes({0x48, 0x89, 0xd8, 0x48, 0x83, 0xc0, 0x02, 0xc3});
  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  llvm::Function *Function =
      LoweredOrErr->module().getFunction(LoweredOrErr->blockSymbol());
  ASSERT_NE(Function, nullptr);
  EXPECT_TRUE(llvm::any_of(
      llvm::instructions(*Function), [](const llvm::Instruction &Instruction) {
        return Instruction.getOpcode() == llvm::Instruction::Add;
      }));
  expectValidRuntimeIR(LoweredOrErr->module());
}

TEST(X86TranslationBlockLowerer, LowersFullWidthMoveImmediate) {
  TranslationBlockDescriptorV1 Block = blockFromBytes(
      {0x48, 0xb8, 0x78, 0x56, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0xc3});
  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  expectValidRuntimeIR(LoweredOrErr->module());
}

TEST(X86TranslationBlockLowerer, LowersSubtractAndSignedBorrowFlags) {
  TranslationBlockDescriptorV1 Block =
      blockFromBytes({0x48, 0x89, 0xf8, 0x48, 0x83, 0xe8, 0x02, 0xc3});
  ASSERT_TRUE(llvm::any_of(Block.Ops, [](const LowOp &Operation) {
    return Operation.Opcode == NdOp::INT_SUB;
  }));
  ASSERT_TRUE(llvm::any_of(Block.Ops, [](const LowOp &Operation) {
    return Operation.Opcode == NdOp::INT_SBOR;
  }));

  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  expectValidRuntimeIR(LoweredOrErr->module());
}

TEST(X86TranslationBlockLowerer, LowersRegisterAddAndSubtractForms) {
  TranslationBlockDescriptorV1 Block = blockFromBytes(
      {0x48, 0x89, 0xd8, 0x48, 0x01, 0xc8, 0x48, 0x29, 0xd0, 0xc3});
  EXPECT_EQ(llvm::count_if(Block.Ops,
                           [](const LowOp &Operation) {
                             return Operation.Opcode == NdOp::INT_ADD;
                           }),
            1u);
  EXPECT_EQ(llvm::count_if(Block.Ops,
                           [](const LowOp &Operation) {
                             return Operation.Opcode == NdOp::INT_SUB;
                           }),
            1u);

  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  expectValidRuntimeIR(LoweredOrErr->module());
}

TEST(X86TranslationBlockLowerer, LowersReturnImmediateStackAdjustment) {
  TranslationBlockDescriptorV1 Block =
      blockFromBytes({0x48, 0x89, 0xf8, 0xc2, 0x10, 0x00});
  ASSERT_TRUE(hasTranslationBlockDescriptorFlag(
      Block.Header.Flags,
      TranslationBlockDescriptorFlagV1::HasReturnImmediate));
  ASSERT_EQ(Block.Header.ReturnImmediate, 0x10u);

  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  expectValidRuntimeIR(LoweredOrErr->module());
}

TEST(X86TranslationBlockLowerer,
     MatchesIndependentMoveTruthVectorIncludingExtendedRegisters) {
  constexpr uint64_t Value = 0xfedcba9876543210ULL;
  std::vector<uint8_t> Bytes;
  appendMovImmediate(Bytes, RuntimeX86_64GPRV1::R8, Value);
  Bytes.insert(Bytes.end(), {0x4d, 0x89, 0xc1}); // mov r9, r8
  Bytes.push_back(0xc3);

  TranslationBlockDescriptorV1 Block = blockFromBytes(Bytes);
  EXPECT_EQ(Block.Bytes, Bytes);
  llvm::LLVMContext Context;
  llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
      lowerX86TranslationBlockV1(Block, resolvedAArch64(), aarch64DataLayout(),
                                 Context);
  ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
      << llvm::toString(LoweredOrErr.takeError());
  llvm::Function *Function =
      LoweredOrErr->module().getFunction(LoweredOrErr->blockSymbol());
  ASSERT_NE(Function, nullptr);
  llvm::CallInst *ReturnLoad = findCall(*Function, [](llvm::CallInst &Call) {
    const llvm::Function *Callee = Call.getCalledFunction();
    return Callee && Callee->getName() == "nvd_rt_v1_load64_le";
  });
  ASSERT_NE(ReturnLoad, nullptr);
  const llvm::DataLayout &Layout = LoweredOrErr->module().getDataLayout();
  expectStateIntegerBefore(*ReturnLoad, Function->getArg(0), Layout,
                           gprOffset(RuntimeX86_64GPRV1::R8), 64, Value);
  expectStateIntegerBefore(*ReturnLoad, Function->getArg(0), Layout,
                           gprOffset(RuntimeX86_64GPRV1::R9), 64, Value);
  expectValidRuntimeIR(LoweredOrErr->module());
}

TEST(X86TranslationBlockLowerer,
     MatchesIndependentRegisterArithmeticFlagTruthVectors) {
  struct TruthVector {
    const char *Name;
    uint64_t Left;
    uint64_t Right;
    ReferenceArithmetic Operation;
    std::array<uint8_t, 3> Instruction;
  };
  constexpr std::array<TruthVector, 9> Vectors = {{
      {"add-unsigned-wrap",
       std::numeric_limits<uint64_t>::max(),
       1,
       ReferenceArithmetic::Add,
       {0x48, 0x01, 0xd8}},
      {"add-positive-overflow",
       std::numeric_limits<int64_t>::max(),
       1,
       ReferenceArithmetic::Add,
       {0x48, 0x01, 0xd8}},
      {"add-negative-overflow",
       uint64_t{1} << 63,
       uint64_t{1} << 63,
       ReferenceArithmetic::Add,
       {0x48, 0x01, 0xd8}},
      {"add-low-byte-even-parity",
       1,
       2,
       ReferenceArithmetic::Add,
       {0x48, 0x01, 0xd8}},
      {"add-low-byte-odd-parity",
       0,
       1,
       ReferenceArithmetic::Add,
       {0x48, 0x01, 0xd8}},
      {"sub-unsigned-borrow",
       0,
       1,
       ReferenceArithmetic::Sub,
       {0x48, 0x29, 0xd8}},
      {"sub-negative-overflow",
       uint64_t{1} << 63,
       1,
       ReferenceArithmetic::Sub,
       {0x48, 0x29, 0xd8}},
      {"sub-positive-minus-negative",
       std::numeric_limits<int64_t>::max(),
       std::numeric_limits<uint64_t>::max(),
       ReferenceArithmetic::Sub,
       {0x48, 0x29, 0xd8}},
      {"sub-zero-result", 1, 1, ReferenceArithmetic::Sub, {0x48, 0x29, 0xd8}},
  }};

  for (const TruthVector &Vector : Vectors) {
    SCOPED_TRACE(Vector.Name);
    const std::vector<uint8_t> Bytes =
        registerArithmeticBlock(Vector.Left, Vector.Right, Vector.Instruction);
    expectArithmeticTruth(Bytes, Vector.Left, Vector.Right, Vector.Operation);
  }
}

TEST(X86TranslationBlockLowerer,
     SignExtendsNegativeImmediateTruthVectorsBeforeUpdatingFlags) {
  struct TruthVector {
    const char *Name;
    uint64_t Left;
    uint64_t SignExtendedImmediate;
    ReferenceArithmetic Operation;
    llvm::ArrayRef<uint8_t> Instruction;
  };
  constexpr std::array<uint8_t, 4> AddNegativeImm8 = {0x48, 0x83, 0xc0, 0xff};
  constexpr std::array<uint8_t, 4> SubNegativeImm8 = {0x48, 0x83, 0xe8, 0x80};
  constexpr std::array<uint8_t, 6> AddNegativeImm32 = {0x48, 0x05, 0xff,
                                                       0xff, 0xff, 0xff};
  constexpr std::array<uint8_t, 6> SubNegativeImm32 = {0x48, 0x2d, 0x00,
                                                       0x00, 0x00, 0x80};
  const std::array<TruthVector, 4> Vectors = {{
      {"add-minus-one-imm8", 0, std::numeric_limits<uint64_t>::max(),
       ReferenceArithmetic::Add, AddNegativeImm8},
      {"sub-minus-128-imm8", 0x7f,
       static_cast<uint64_t>(static_cast<int64_t>(-128)),
       ReferenceArithmetic::Sub, SubNegativeImm8},
      {"add-minus-one-imm32", uint64_t{1} << 63,
       std::numeric_limits<uint64_t>::max(), ReferenceArithmetic::Add,
       AddNegativeImm32},
      {"sub-int32-min-imm32", std::numeric_limits<int64_t>::max(),
       static_cast<uint64_t>(
           static_cast<int64_t>(std::numeric_limits<int32_t>::min())),
       ReferenceArithmetic::Sub, SubNegativeImm32},
  }};

  for (const TruthVector &Vector : Vectors) {
    SCOPED_TRACE(Vector.Name);
    const std::vector<uint8_t> Bytes =
        immediateArithmeticBlock(Vector.Left, Vector.Instruction);
    expectArithmeticTruth(Bytes, Vector.Left, Vector.SignExtendedImmediate,
                          Vector.Operation);
  }
}

TEST(X86TranslationBlockLowerer,
     MatchesIndependentLogicalOperationAndFlagTruthVectors) {
  struct TruthVector {
    const char *Name;
    uint64_t Left;
    uint64_t Right;
    ReferenceLogic Operation;
    std::array<uint8_t, 3> Instruction;
  };
  constexpr std::array<TruthVector, 6> Vectors = {{
      {"and-masks-bits",
       0xff00ff00ff00ff00ULL,
       0x0f0f0f0f0f0f0f0fULL,
       ReferenceLogic::And,
       {0x48, 0x21, 0xd8}},
      {"and-zero",
       0xaaaaaaaaaaaaaaaaULL,
       0x5555555555555555ULL,
       ReferenceLogic::And,
       {0x48, 0x21, 0xd8}},
      {"or-sign",
       uint64_t{1} << 63,
       0x7f,
       ReferenceLogic::Or,
       {0x48, 0x09, 0xd8}},
      {"or-low-byte-even-parity", 1, 2, ReferenceLogic::Or, {0x48, 0x09, 0xd8}},
      {"xor-zero",
       0x123456789abcdef0ULL,
       0x123456789abcdef0ULL,
       ReferenceLogic::Xor,
       {0x48, 0x31, 0xd8}},
      {"xor-low-byte-odd-parity",
       0,
       1,
       ReferenceLogic::Xor,
       {0x48, 0x31, 0xd8}},
  }};

  for (const TruthVector &Vector : Vectors) {
    SCOPED_TRACE(Vector.Name);
    const std::vector<uint8_t> Bytes =
        registerArithmeticBlock(Vector.Left, Vector.Right, Vector.Instruction);
    expectLogicTruth(Bytes, Vector.Left, Vector.Right, Vector.Operation);
  }
}

TEST(X86TranslationBlockLowerer,
     HandlesLogicalIdiomAndSignExtendedImmediateForms) {
  std::vector<uint8_t> ZeroExtendedRegister;
  appendMovImmediate(ZeroExtendedRegister, RuntimeX86_64GPRV1::R8,
                     0xffffffffffffffffULL);
  ZeroExtendedRegister.insert(ZeroExtendedRegister.end(),
                              {0x4d, 0x31, 0xc0}); // xor r8, r8
  ZeroExtendedRegister.push_back(0xc3);
  expectLogicTruth(ZeroExtendedRegister, 0xffffffffffffffffULL,
                   0xffffffffffffffffULL, ReferenceLogic::Xor,
                   RuntimeX86_64GPRV1::R8);

  constexpr std::array<uint8_t, 4> OrNegativeImm8 = {0x48, 0x83, 0xc8, 0xff};
  const std::vector<uint8_t> Immediate =
      immediateArithmeticBlock(0, OrNegativeImm8);
  expectLogicTruth(Immediate, 0, std::numeric_limits<uint64_t>::max(),
                   ReferenceLogic::Or);

  const std::vector<uint8_t> AndSame = registerArithmeticBlock(
      0x8040201008040201ULL, 0x9999999999999999ULL,
      std::array<uint8_t, 3>{0x48, 0x21, 0xc0}); // and rax, rax
  expectLogicTruth(AndSame, 0x8040201008040201ULL, 0x8040201008040201ULL,
                   ReferenceLogic::And);
}

TEST(X86TranslationBlockLowerer,
     ModelsReturnFaultAndSuccessStateForRetAndRetImmediate) {
  expectReturnTruth(std::numeric_limits<uint64_t>::max() - 7, 0);
  expectReturnTruth(0x1000, 0x1234);
}

TEST(X86TranslationBlockLowerer,
     RejectsNonCanonicalWidthMemoryAndPrefixScalarForms) {
  const std::array<std::vector<uint8_t>, 5> Encodings = {{
      {0x89, 0xd8, 0xc3},                   // mov eax, ebx
      {0x48, 0x8b, 0x00, 0xc3},             // mov rax, [rax]
      {0x48, 0x48, 0x01, 0xd8, 0xc3},       // repeated REX
      {0x66, 0x48, 0x83, 0xc0, 0x01, 0xc3}, // legacy prefix
      {0x66, 0xc2, 0x10, 0x00},             // prefixed ret iw
  }};

  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    llvm::Expected<TranslationBlockDescriptorV1> BlockOrErr =
        tryBlockFromBytes(Bytes);
    ASSERT_TRUE(static_cast<bool>(BlockOrErr))
        << llvm::toString(BlockOrErr.takeError());
    llvm::LLVMContext Context;
    expectLoweringError(
        lowerX86TranslationBlockV1(*BlockOrErr, resolvedAArch64(),
                                   aarch64DataLayout(), Context),
        TranslationBlockLoweringErrorCode::UnsupportedBlockShape);
  }
}

TEST(X86TranslationBlockLowerer, RejectsSemanticallyRedundantREXExtensionBits) {
  const std::array<std::vector<uint8_t>, 3> Encodings = {{
      {0x4a, 0x89, 0xd8, 0xc3}, // REX.X is ignored by register-direct MOV.
      {0x4c, 0x83, 0xc0, 0x01,
       0xc3}, // REX.R is ignored by the ADD opcode-extension field.
      {0x49, 0x05, 0x01, 0x00, 0x00, 0x00,
       0xc3}, // REX.B is ignored by accumulator-immediate ADD.
  }};

  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    llvm::Expected<TranslationBlockDescriptorV1> BlockOrErr =
        tryBlockFromBytes(Bytes);
    ASSERT_TRUE(static_cast<bool>(BlockOrErr))
        << llvm::toString(BlockOrErr.takeError());
    llvm::LLVMContext Context;
    expectLoweringError(
        lowerX86TranslationBlockV1(*BlockOrErr, resolvedAArch64(),
                                   aarch64DataLayout(), Context),
        TranslationBlockLoweringErrorCode::UnsupportedBlockShape);
  }
}

TEST(X86TranslationBlockLowerer, AcceptsNecessaryREXExtensionBits) {
  const std::array<std::vector<uint8_t>, 3> Encodings = {{
      {0x4d, 0x89, 0xc1, 0xc3}, // mov r9, r8: REX.R and REX.B are required.
      {0x49, 0x83, 0xc0, 0x01, 0xc3}, // add r8, 1: REX.B is required.
      {0x49, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0xc3}, // movabs r8, 1: REX.B extends the opcode register.
  }};

  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    TranslationBlockDescriptorV1 Block = blockFromBytes(Bytes);
    llvm::LLVMContext Context;
    llvm::Expected<LoweredTranslationBlockV1> LoweredOrErr =
        lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                   aarch64DataLayout(), Context);
    ASSERT_TRUE(static_cast<bool>(LoweredOrErr))
        << llvm::toString(LoweredOrErr.takeError());
    expectValidRuntimeIR(LoweredOrErr->module());
  }
}

TEST(X86TranslationBlockLowerer,
     RejectsPrefixedAndUnpublishedControlFormsWithoutExpandingTheSlice) {
  const std::array<std::vector<uint8_t>, 14> Encodings = {{
      {0x66, 0xeb, 0x00}, // legacy-prefix direct jump
      {0x40, 0xeb, 0x00}, // REX-prefixed direct jump
      {0x66, 0x74, 0x00}, // legacy-prefix zero-flag branch
      {0x40, 0x75, 0x00}, // REX-prefixed zero-flag branch
      {0x66, 0x70, 0x00}, // legacy-prefix overflow branch
      {0x40, 0x73, 0x00}, // REX-prefixed carry branch
      {0x66, 0x0f, 0x88, 0x00, 0x00, 0x00, 0x00}, // prefixed near sign
      {0x40, 0x0f, 0x8b, 0x00, 0x00, 0x00, 0x00}, // REX near parity
      {0x66, 0x76, 0x00}, // legacy-prefix multi-flag branch
      {0x40, 0x77, 0x00}, // REX-prefixed multi-flag branch
      {0x66, 0x0f, 0x8c, 0x00, 0x00, 0x00,
       0x00}, // legacy-prefix near multi-flag branch
      {0x40, 0x0f, 0x8f, 0x00, 0x00, 0x00, 0x00}, // REX near multi-flag branch
      {0xff, 0xe0},                               // indirect jump through rax
      {0xe8, 0x00, 0x00, 0x00, 0x00}              // direct call
  }};

  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    llvm::Expected<TranslationBlockDescriptorV1> BlockOrErr =
        tryBlockFromBytes(Bytes);
    ASSERT_TRUE(static_cast<bool>(BlockOrErr))
        << llvm::toString(BlockOrErr.takeError());
    llvm::LLVMContext Context;
    expectLoweringError(
        lowerX86TranslationBlockV1(*BlockOrErr, resolvedAArch64(),
                                   aarch64DataLayout(), Context),
        TranslationBlockLoweringErrorCode::UnsupportedBlockShape);
  }
}

TEST(X86TranslationBlockLowerer,
     RejectsCountedAndLoopConditionalBranchEncodings) {
  const std::array<std::vector<uint8_t>, 5> Encodings = {{
      {0xe3, 0x00},
      {0x67, 0xe3, 0x00},
      {0xe0, 0x00},
      {0xe1, 0x00},
      {0xe2, 0x00},
  }};

  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    TranslationBlockDescriptorV1 Block = blockFromBytes(Bytes);
    ASSERT_EQ(Block.Header.Terminator,
              TranslationBlockTerminatorKindV1::ConditionalBranch);
    llvm::LLVMContext Context;
    expectLoweringError(
        lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                   aarch64DataLayout(), Context),
        TranslationBlockLoweringErrorCode::UnsupportedBlockShape);
  }
}

TEST(X86TranslationBlockLowerer,
     RejectsDirectJumpTargetThatDisagreesWithGuestBytes) {
  TranslationBlockDescriptorV1 Block = blockFromBytes({0xeb, 0x05});
  ASSERT_EQ(Block.Header.StaticTargetPC, EntryPC + 7);
  ++Block.Header.StaticTargetPC;
  Block.InstructionBoundaries.back().Immediate = Block.Header.StaticTargetPC;
  ASSERT_FALSE(Block.Ops.empty());
  ASSERT_EQ(Block.Ops.back().Opcode, NdOp::BRANCH);
  Block.Ops.back().Inputs[0].Offset = Block.Header.StaticTargetPC;
  ASSERT_FALSE(validateTranslationBlockDescriptorV1(Block));

  llvm::LLVMContext Context;
  expectLoweringError(lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                                 aarch64DataLayout(), Context),
                      TranslationBlockLoweringErrorCode::InvalidDescriptor);
}

TEST(X86TranslationBlockLowerer,
     RejectsUndeclaredGuestOpcodeEvenWhenItsLowIRIsLowerable) {
  TranslationBlockDescriptorV1 Block =
      blockFromBytes({0x48, 0x85, 0xd8, 0xc3}); // test rax, rbx; ret
  ASSERT_TRUE(llvm::any_of(Block.Ops, [](const LowOp &Operation) {
    return Operation.Opcode == NdOp::INT_AND;
  }));

  llvm::LLVMContext Context;
  expectLoweringError(lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                                 aarch64DataLayout(), Context),
                      TranslationBlockLoweringErrorCode::UnsupportedBlockShape);
}

TEST(X86TranslationBlockLowerer, ReportsAnUnsupportedInstructionAtItsBoundary) {
  TranslationBlockDescriptorV1 Block =
      blockFromBytes({0x48, 0x89, 0xf8, 0x90, 0xc3}); // mov rax, rdi; nop; ret
  ASSERT_EQ(Block.InstructionBoundaries.size(), 3u);
  ASSERT_EQ(Block.InstructionBoundaries[1].Address, EntryPC + 3);

  llvm::LLVMContext Context;
  expectLoweringError(lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                                 aarch64DataLayout(), Context),
                      TranslationBlockLoweringErrorCode::UnsupportedBlockShape,
                      std::nullopt, EntryPC + 3);
}

TEST(X86TranslationBlockLowerer, RejectsPrefixedReturnEncodings) {
  for (const std::vector<uint8_t> &Bytes :
       {std::vector<uint8_t>{0x66, 0xc3}, std::vector<uint8_t>{0xf2, 0xc3},
        std::vector<uint8_t>{0x40, 0xc3}}) {
    llvm::Expected<TranslationBlockDescriptorV1> BlockOrErr =
        tryBlockFromBytes(Bytes);
    if (!BlockOrErr) {
      llvm::consumeError(BlockOrErr.takeError());
      continue;
    }
    llvm::LLVMContext Context;
    expectLoweringError(
        lowerX86TranslationBlockV1(*BlockOrErr, resolvedAArch64(),
                                   aarch64DataLayout(), Context),
        TranslationBlockLoweringErrorCode::UnsupportedBlockShape);
  }
}

TEST(X86TranslationBlockLowerer, RejectsMutatedLowIROperationBeforeLowering) {
  TranslationBlockDescriptorV1 Block = fixtureBlock();
  ASSERT_FALSE(Block.Ops.empty());
  Block.Ops.front().Opcode = NdOp::FLOAT_ADD;
  llvm::LLVMContext Context;
  expectLoweringError(lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                                 aarch64DataLayout(), Context),
                      TranslationBlockLoweringErrorCode::InvalidDescriptor);
}

TEST(X86TranslationBlockLowerer, RejectsInvalidDescriptorBeforeLowering) {
  TranslationBlockDescriptorV1 Block = fixtureBlock();
  ++Block.Header.Version;
  llvm::LLVMContext Context;
  expectLoweringError(lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                                 aarch64DataLayout(), Context),
                      TranslationBlockLoweringErrorCode::InvalidDescriptor);
}

TEST(X86TranslationBlockLowerer, RejectsLowIRThatDisagreesWithGuestBytes) {
  TranslationBlockDescriptorV1 Block = fixtureBlock();
  bool Mutated = false;
  for (LowOp &Operation : Block.Ops) {
    if (Operation.Opcode != NdOp::INT_ADD)
      continue;
    for (uint8_t I = 0; I < Operation.NumInputs; ++I) {
      if (!Operation.Inputs[I].isConst() || Operation.Inputs[I].Offset != 1)
        continue;
      Operation.Inputs[I].Offset = 2;
      Mutated = true;
      break;
    }
    if (Mutated)
      break;
  }
  ASSERT_TRUE(Mutated);

  llvm::LLVMContext Context;
  expectLoweringError(lowerX86TranslationBlockV1(Block, resolvedAArch64(),
                                                 aarch64DataLayout(), Context),
                      TranslationBlockLoweringErrorCode::InvalidDescriptor);
}

TEST(X86TranslationBlockLowerer, RejectsNonAArch64DataLayout) {
  llvm::LLVMContext Context;
  expectLoweringError(
      lowerX86TranslationBlockV1(fixtureBlock(), resolvedAArch64(),
                                 llvm::DataLayout("e-p:32:32-i64:64-n32-S128"),
                                 Context),
      TranslationBlockLoweringErrorCode::InvalidHostDataLayout);
}

TEST(X86TranslationBlockLowerer,
     RejectsPlausibleButNonCanonicalAArch64DataLayout) {
  const llvm::DataLayout PlausibleLayout("e-p:64:64-i64:64-n32:64-S128");
  ASSERT_NE(PlausibleLayout.getStringRepresentation(),
            aarch64DataLayout().getStringRepresentation());
  llvm::LLVMContext Context;
  expectLoweringError(lowerX86TranslationBlockV1(fixtureBlock(),
                                                 resolvedAArch64(),
                                                 PlausibleLayout, Context),
                      TranslationBlockLoweringErrorCode::InvalidHostDataLayout);
}

TEST(X86TranslationBlockLowerer,
     RejectsResolvedTargetWithoutATargetMachineLayoutBinding) {
  ResolvedHostTarget Unbound =
      llvm::cantFail(resolveHostTarget(aarch64AOTOptions()));
  ASSERT_FALSE(Unbound.hasCanonicalDataLayout());
  llvm::LLVMContext Context;
  expectLoweringError(lowerX86TranslationBlockV1(fixtureBlock(), Unbound,
                                                 aarch64DataLayout(), Context),
                      TranslationBlockLoweringErrorCode::InvalidHostDataLayout);
}

} // namespace
