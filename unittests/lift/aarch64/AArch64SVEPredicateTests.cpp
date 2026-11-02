//===- AArch64SVEPredicateTests.cpp - Canonical predicate state -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent PTRUE words and cross-element predicate consumers exercise
/// the full svbool carrier through the production LLVM emission boundary.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/Verifier.h"

#include <array>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

#define NEVERD_SVE_FIXTURE_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64SVEPredicateFixtures.def"

std::vector<LowOp> lift(uint32_t Word, bool Fast, bool Strict = true) {
  Decoder Decode;
  EXPECT_TRUE(Decode.init(Arch::AArch64));
  Decode.setStrict(Strict);
  std::array<uint8_t, kInstructionBytes> Bytes;
  writeLE<uint32_t>(Bytes.data(), Word);
  DecodedInsn Instruction;
  EXPECT_EQ(
      Fast ? Decode.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry,
                                     Instruction)
           : Decode.decodeOne(Bytes.data(), Bytes.size(), kEntry, Instruction),
      Bytes.size());
  std::vector<LowOp> Operations;
  Decode.liftToLow(Instruction, Operations);
  return Operations;
}

MedVar temporary(unsigned Identifier, uint16_t Bytes) {
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.TheArch = Arch::AArch64;
  Value.Id = Identifier;
  Value.Size = Bytes;
  return Value;
}

MedOp intrinsic(Intrinsic Identifier, MedVar Output,
                std::initializer_list<MedVar> Inputs) {
  MedOp Operation;
  Operation.Opcode = NdOp::INTRINSIC;
  Operation.Output = Output;
  Operation.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Identifier), kIdentifierBytes));
  for (const auto &Input : Inputs)
    Operation.addInput(Input);
  return Operation;
}

MedFunc predicateFunction(unsigned ProducerBytes, unsigned ConsumerBytes,
                          bool Store) {
  MedFunc Function;
  Function.Name = kFunction;
  Function.Entry = kEntry;
  Function.ReturnType = NdType::makeInt(kScalarBytes, false);
  const auto Predicate = temporary(0, kPredicateBytes);
  const auto Vector = temporary(1, kVectorBytes);
  const auto Result = temporary(2, kScalarBytes);
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.back();
  Block.Id = 0;
  Block.StartAddr = kEntry;
  Block.Ops.push_back(
      intrinsic(Intrinsic::A64_SvePtrue, Predicate,
                {MedVar::makeConst(kPattern, kPatternBytes),
                 MedVar::makeConst(ProducerBytes, kElementBytes)}));
  Block.Ops.push_back(
      intrinsic(Intrinsic::A64_SveIndex, Vector,
                {MedVar::makeConst(kIndexStart, kScalarBytes),
                 MedVar::makeConst(kIndexStep, kScalarBytes),
                 MedVar::makeConst(ConsumerBytes, kElementBytes)}));
  if (Store) {
    MedVar Address;
    Address.Kind = MedVar::Param;
    Address.TheArch = Arch::AArch64;
    Address.Size = kScalarBytes;
    Function.Params.push_back(Address);
    Block.Ops.push_back(
        intrinsic(Intrinsic::A64_SveSt1, {},
                  {Predicate, Vector, Address,
                   MedVar::makeConst(ConsumerBytes, kElementBytes),
                   MedVar::makeConst(ConsumerBytes, kElementBytes)}));
  }
  Block.Ops.push_back(intrinsic(
      Intrinsic::A64_SveLastb, Result,
      {Predicate, Vector, MedVar::makeConst(ConsumerBytes, kElementBytes)}));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Result);
  Block.Ops.push_back(Return);
  return Function;
}

TEST(AArch64SVEPredicate, RawPtruePreservesPatternWidthAndRegister) {
  struct Case {
    uint32_t Word;
    unsigned Register;
    unsigned Pattern;
    unsigned Element;
  };
  const Case Cases[] = {
#define NEVERD_SVE_FIXTURE_PTRUE(Word, Register, Pattern, Element)             \
  {Word, Register, Pattern, Element},
#include "AArch64SVEPredicateFixtures.def"
  };
  for (bool Fast : {false, true})
    for (const auto &Case : Cases) {
      SCOPED_TRACE(Case.Word);
      const auto Operations = lift(Case.Word, Fast);
      ASSERT_EQ(Operations.size(), 1u);
      const auto &Operation = Operations.front();
      EXPECT_TRUE(lowAArch64PtrueShapeIsValid(Operation));
      ASSERT_EQ(Operation.Opcode, NdOp::INTRINSIC);
      ASSERT_EQ(Operation.NumInputs, 3);
      EXPECT_EQ(Operation.Inputs[0].Offset,
                static_cast<uint64_t>(Intrinsic::A64_SvePtrue));
      EXPECT_EQ(Operation.Output.Offset, a64reg::P(Case.Register));
      EXPECT_EQ(Operation.Output.Size, kPredicateBytes);
      EXPECT_EQ(Operation.Inputs[1].Offset, Case.Pattern);
      EXPECT_EQ(Operation.Inputs[1].Size, kPatternBytes);
      EXPECT_EQ(Operation.Inputs[2].Offset, Case.Element);
      EXPECT_EQ(Operation.Inputs[2].Size, kElementBytes);
    }
}

TEST(AArch64SVEPredicate, IncompletePrefixAndFlagSemanticsRemainRefused) {
  const uint32_t Words[] = {
#define NEVERD_SVE_FIXTURE_REJECT(Word) Word,
#include "AArch64SVEPredicateFixtures.def"
  };
  for (bool Fast : {false, true})
    for (bool Strict : {false, true})
      for (const auto Word : Words) {
        SCOPED_TRACE(Word);
        EXPECT_THROW(lift(Word, Fast, Strict), UnliftedInstruction);
      }
}

TEST(AArch64SVEPredicate, MalformedPtrueFailsEveryIRAdmissionBoundary) {
  const auto Valid = predicateFunction(kByteElement, kByteElement, false);
  ASSERT_TRUE(verifyMedFunc(Valid, kFunction));
  std::vector<MedFunc> Malformed;
  struct Case {
    uint64_t Pattern;
    uint64_t Element;
    uint16_t PatternBytes;
    uint16_t ElementBytes;
    uint16_t OutputBytes;
  };
  const Case Cases[] = {
#define NEVERD_SVE_FIXTURE_BAD_SHAPE(Pattern, Element, PatternBytes,           \
                                     ElementBytes, OutputBytes)                \
  {Pattern, Element, PatternBytes, ElementBytes, OutputBytes},
#include "AArch64SVEPredicateFixtures.def"
  };
  for (const auto &Case : Cases) {
    auto Bad = Valid;
    auto &Op = Bad.Blocks[0].Ops[0];
    Op.Inputs[1] = MedVar::makeConst(Case.Pattern, Case.PatternBytes);
    Op.Inputs[2] = MedVar::makeConst(Case.Element, Case.ElementBytes);
    Op.Output.Size = Case.OutputBytes;
    Malformed.push_back(std::move(Bad));
  }
  for (unsigned Index : {1u, 2u}) {
    auto Dynamic = Valid;
    auto &Op = Dynamic.Blocks[0].Ops[0];
    Op.Inputs[Index] = temporary(Index, Op.Inputs[Index].Size);
    Malformed.push_back(std::move(Dynamic));
  }
  auto Missing = Valid;
  --Missing.Blocks[0].Ops[0].NumInputs;
  Malformed.push_back(std::move(Missing));
  auto Extra = Valid;
  Extra.Blocks[0].Ops[0].addInput(MedVar::makeConst(0, kElementBytes));
  Malformed.push_back(std::move(Extra));
  auto ConstantOutput = Valid;
  ConstantOutput.Blocks[0].Ops[0].Output =
      MedVar::makeConst(0, kPredicateBytes);
  Malformed.push_back(std::move(ConstantOutput));
  auto Ordered = Valid;
  Ordered.Blocks[0].Ops[0].MemoryOrdering = NdMemoryOrdering::Acquire;
  Malformed.push_back(std::move(Ordered));

  for (const auto &Bad : Malformed) {
    const auto &Op = Bad.Blocks[0].Ops[0];
    EXPECT_FALSE(aarch64PtrueMedShapeIsValid(Op));
    EXPECT_FALSE(verifyMedFunc(Bad, kFunction));
    LowBlock Block;
    Block.StartAddr = kEntry;
    Block.EndAddr = kEntry + kInstructionBytes;
    LowOp Low;
    Low.Opcode = Op.Opcode;
    Low.MemoryOrdering = Op.MemoryOrdering;
    Low.Output = Op.Output.isConst() ? NdVar::cst(0, Op.Output.Size)
                                     : NdVar::tmp(0, Op.Output.Size);
    for (unsigned Index = 0; Index != Op.NumInputs; ++Index) {
      const auto &Input = Op.Inputs[Index];
      Low.addInput(Input.isConst()
                       ? NdVar::cst(Input.ConstVal,
                                    Index ? Input.Size : kLowIdentifierBytes)
                       : NdVar::tmp(Index, Input.Size));
    }
    Block.Ops.push_back(Low);
    LowInstructionBoundary Boundary;
    Boundary.Address = kEntry;
    Boundary.Size = kInstructionBytes;
    Boundary.OpCount = 1;
    Block.InstructionBoundaries.push_back(Boundary);
    EXPECT_FALSE(lowAArch64PtrueShapeIsValid(Low));
    auto Error = validateLowInstructionBoundaries(Block);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    llvm::LLVMContext Context;
    EXPECT_THROW(
        MedLLVMEmitter().emit({Bad}, Context, kFunction, Arch::AArch64),
        std::runtime_error);
  }
  auto Auxiliary = Valid;
  auto &Op = Auxiliary.Blocks[0].Ops[0];
  Op.IntrinsicOutputs.push_back(Op.Output);
  EXPECT_FALSE(aarch64PtrueMedShapeIsValid(Op));
  EXPECT_FALSE(verifyMedFunc(Auxiliary, kFunction));
  llvm::LLVMContext Context;
  const std::vector<char> Mask{false};
  EXPECT_THROW(MedLLVMEmitter().emit({Auxiliary}, Context, kFunction,
                                     Arch::AArch64, {}, nullptr,
                                     BinaryFormat::ELF, false, &Mask),
               std::runtime_error);
  EXPECT_THROW(MedLLVMEmitter().emit({Valid}, Context, kFunction, Arch::X64),
               std::runtime_error);
}

TEST(AArch64SVEPredicate, LLVMStorageUsesFullPredicateAcrossElementWidths) {
  const unsigned Widths[] = {
#define NEVERD_SVE_FIXTURE_WIDTH(Bytes) Bytes,
#include "AArch64SVEPredicateFixtures.def"
  };
  for (unsigned Producer : Widths)
    for (unsigned Consumer : Widths)
      for (bool Store : {false, true}) {
        SCOPED_TRACE(testing::Message() << Producer << '/' << Consumer);
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit(
            {predicateFunction(Producer, Consumer, Store)}, Context, kFunction,
            Arch::AArch64);
        ASSERT_TRUE(Module);
        ASSERT_FALSE(llvm::verifyModule(*Module));
        unsigned ToFull = 0;
        unsigned FromFull = 0;
        for (const auto &Block : *Module->getFunction(kFunction))
          for (const auto &Instruction : Block) {
            llvm::Type *StoredType = nullptr;
            if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
              StoredType = Load->getType();
            if (const auto *Save =
                    llvm::dyn_cast<llvm::StoreInst>(&Instruction))
              StoredType = Save->getValueOperand()->getType();
            if (const auto *Vector =
                    llvm::dyn_cast_or_null<llvm::ScalableVectorType>(
                        StoredType))
              if (Vector->getElementType()->isIntegerTy(1))
                EXPECT_EQ(Vector->getElementCount().getKnownMinValue(),
                          kCanonicalLanes);
            if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
              if (const auto *Callee = Call->getCalledFunction()) {
                ToFull += Callee->getIntrinsicID() ==
                          llvm::Intrinsic::aarch64_sve_convert_to_svbool;
                FromFull += Callee->getIntrinsicID() ==
                            llvm::Intrinsic::aarch64_sve_convert_from_svbool;
              }
          }
        EXPECT_EQ(ToFull, Producer == kByteElement ? 0u : 1u);
        EXPECT_EQ(FromFull, Consumer == kByteElement ? 0u : (Store ? 2u : 1u));
      }
}

TEST(AArch64SVEPredicate, EveryEncodedPatternIsAdmittedWithoutClamping) {
  for (uint64_t Pattern = 0; Pattern != kInvalidPattern; ++Pattern) {
    auto Function = predicateFunction(kByteElement, kByteElement, false);
    auto &Op = Function.Blocks[0].Ops[0];
    Op.Inputs[1] = MedVar::makeConst(Pattern, kPatternBytes);
    ASSERT_TRUE(aarch64PtrueMedShapeIsValid(Op));
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kFunction, Arch::AArch64);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    bool Found = false;
    for (const auto &Block : *Module->getFunction(kFunction))
      for (const auto &Instruction : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          if (const auto *Callee = Call->getCalledFunction())
            if (Callee->getIntrinsicID() ==
                llvm::Intrinsic::aarch64_sve_ptrue) {
              const auto *Operand =
                  llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(0));
              ASSERT_TRUE(Operand);
              EXPECT_EQ(Operand->getZExtValue(), Pattern);
              Found = true;
            }
    EXPECT_TRUE(Found);
  }
}

TEST(AArch64SVEPredicate, CanonicalPredicateBridgeCompilesForNativeSVE) {
  const std::pair<unsigned, unsigned> Cases[] = {
#define NEVERD_SVE_FIXTURE_NATIVE(Producer, Consumer) {Producer, Consumer},
#include "AArch64SVEPredicateFixtures.def"
  };
  for (const auto &[Producer, Consumer] : Cases) {
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({predicateFunction(Producer, Consumer, true)},
                              Context, kFunction, Arch::AArch64);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    const auto Object =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::ELF, kTriple);
    EXPECT_TRUE(Object.Success);
    EXPECT_FALSE(Object.ObjectData.empty());
  }
}

TEST(AArch64SVEPredicate, HighCPreservesEncodedPredicatePatterns) {
  struct Case {
    const char *Pattern;
    const char *Element;
    const char *Expected;
  };
  const Case Cases[] = {
#define NEVERD_SVE_FIXTURE_C(Pattern, Element, Expected)                       \
  {Pattern, Element, Expected},
#include "AArch64SVEPredicateFixtures.def"
  };
  for (const auto &Case : Cases) {
    bool UsesIntrinsics = false;
    EXPECT_EQ(renderARMIntrinsicCall(Intrinsic::A64_SvePtrue,
                                     {Case.Pattern, Case.Element},
                                     kPredicateBytes, UsesIntrinsics),
              Case.Expected);
    EXPECT_TRUE(UsesIntrinsics);
  }
  bool UsesIntrinsics = false;
  EXPECT_TRUE(renderARMIntrinsicCall(Intrinsic::A64_SvePtrue,
                                     {kUnsupportedPattern, Cases[0].Element},
                                     kPredicateBytes, UsesIntrinsics)
                  .empty());
  EXPECT_TRUE(renderARMIntrinsicCall(Intrinsic::A64_SvePtrue,
                                     {Cases[0].Pattern, kMalformedElement},
                                     kPredicateBytes, UsesIntrinsics)
                  .empty());
  for (const auto &Operands :
       {std::vector<std::string>{}, std::vector<std::string>{Cases[0].Pattern},
        std::vector<std::string>{Cases[0].Pattern, Cases[0].Element,
                                 Cases[0].Element},
        std::vector<std::string>{kDynamicOperand, Cases[0].Element},
        std::vector<std::string>{Cases[0].Pattern, kDynamicOperand}})
    EXPECT_TRUE(renderARMIntrinsicCall(Intrinsic::A64_SvePtrue, Operands,
                                       kPredicateBytes, UsesIntrinsics)
                    .empty());
  EXPECT_TRUE(renderARMIntrinsicCall(Intrinsic::A64_SvePtrue,
                                     {Cases[0].Pattern, Cases[0].Element},
                                     kInvalidOutputBytes, UsesIntrinsics)
                  .empty());
}

} // namespace
