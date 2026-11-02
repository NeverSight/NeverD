//===- X86HleSourceTests.cpp - HLE admission and publication -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Raw prefixes, register destinations and unknown native memory effects.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86Hle.h"
#include "X86HleTestCases.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86HleEncoding.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <stdexcept>

using namespace neverd;

TEST(X86HleSource, RawMemoryFormsPreservePrefixesWidthsAndInputs) {
  const auto Cases = test::x86HleTestCases();
  ASSERT_EQ(Cases.size(), 136u);
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Operation);
    SCOPED_TRACE(C.Width);
    const auto E = x86HleEncoding(C.Bytes);
    ASSERT_TRUE(E);
    EXPECT_EQ(unsigned(E->Operation), C.Operation);
    EXPECT_EQ(E->Bytes, C.Width);
    EXPECT_EQ(E->Acquire, C.Acquire);
    EXPECT_EQ(E->Base, 7u);
    ASSERT_TRUE(x86HlePlan(E->control()));
    for (unsigned N = 0; N != C.Bytes.size(); ++N)
      EXPECT_FALSE(x86HleEncoding(llvm::ArrayRef(C.Bytes).take_front(N)));
    auto B = C.Bytes;
    B.push_back(0x90);
    EXPECT_FALSE(x86HleEncoding(B));
    B = C.Bytes;
    B.back() |= 0xc0;
    EXPECT_FALSE(x86HleEncoding(B));
  }
  const auto High = x86HleEncoding({0xf2, 0x0f, 0xc0, 0x27});
  EXPECT_FALSE(High); // XADD requires LOCK.
  const auto AH = x86HleEncoding({0xf2, 0xf0, 0x0f, 0xc0, 0x27});
  ASSERT_TRUE(AH);
  EXPECT_EQ(AH->SourceOffset, 1u);
  EXPECT_EQ(x86HlePlan(AH->control())->Outputs[0].OriginalOffset, 1u);
  EXPECT_EQ(x86HlePlan(AH->control())->Outputs[0].Bytes, 1u);
  const auto SIB = x86HleEncoding(
      {0xf3, 0xf2, 0xf0, 0x65, 0x67, 0x4f, 0x83, 0x6c, 0xac, 0xf0, 0xff});
  ASSERT_TRUE(SIB);
  EXPECT_TRUE(SIB->Acquire);
  EXPECT_EQ(SIB->Base, 12u);
  EXPECT_EQ(SIB->Index, 13u);
  EXPECT_EQ(SIB->Scale, 4u);
  EXPECT_EQ(SIB->Segment, 2u);
  EXPECT_EQ(SIB->AddressBytes, 4u);
  EXPECT_EQ(SIB->Displacement, -16);
  EXPECT_EQ(SIB->Immediate, UINT64_MAX);
  for (const auto &B : {std::vector<uint8_t>{0xf2, 0x89, 0x0f},
                        {0xf3, 0xf0, 0x89, 0x0f},
                        {0xf2, 0xf0, 0xf0, 0x01, 0x0f},
                        {0xf2, 0xf0, 0x48, 0x0f, 0xc7, 0x0f},
                        {0xf2, 0xf0, 0x39, 0x0f}})
    EXPECT_FALSE(x86HleEncoding(B));
}

TEST(X86HleSource, OriginalDestinationsAndControlHaveOneShapeAuthority) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.NumInputs = 6;
  Op.Inputs[0] = NdVar::cst(uint64_t(Intrinsic::X86HleState), 2);
  for (unsigned I = 1; I != 5; ++I)
    Op.Inputs[I] = NdVar::tmp(I, 8);
  Op.Inputs[5] = NdVar::cst(0, 4);
  for (const auto &C : test::x86HleTestCases()) {
    const auto E = x86HleEncoding(C.Bytes);
    ASSERT_TRUE(E);
    Op.Inputs[5].Offset = E->control();
    EXPECT_TRUE(x86HleShapeIsValid(x86HleLowShape(Op, Arch::X64)));
    EXPECT_FALSE(x86HleShapeIsValid(x86HleLowShape(Op, Arch::X86)));
  }
  for (uint64_t C : {0ull, 0x200000000ull, 0x213ull, 0x312ull, 0x5203ull}) {
    Op.Inputs[5].Offset = C;
    EXPECT_FALSE(x86HleShapeIsValid(x86HleLowShape(Op)));
  }
  EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86HleState));
}

TEST(X86HleSource, NativeCallsRejectErasedEffectsAndMalformedControls) {
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext C;
    llvm::Module M("hle", C);
    llvm::IRBuilder<> B(C);
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(),
                                {B.getPtrTy(), B.getPtrTy(), B.getPtrTy(),
                                 B.getInt32Ty(), B.getInt32Ty()},
                                Mutation == 12),
        llvm::GlobalValue::ExternalLinkage, "llvm.x86.hle.context", M);
    for (auto Attr : {llvm::Attribute::Convergent, llvm::Attribute::NoDuplicate,
                      llvm::Attribute::NoUnwind})
      F->addFnAttr(Attr);
    auto *Owner = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), {B.getInt32Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, "owner", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Owner));
    auto *P = llvm::ConstantPointerNull::get(B.getPtrTy());
    auto *Call = B.CreateCall(F, {P, P, P, B.getInt32(0x300), B.getInt32(8)});
    B.CreateRetVoid();
    if (Mutation == 1)
      Call->setOnlyReadsMemory();
    if (Mutation == 2)
      Call->setOnlyWritesMemory();
    if (Mutation == 3)
      Call->setDoesNotReturn();
    if (Mutation == 4)
      F->removeFnAttr(llvm::Attribute::Convergent);
    if (Mutation == 5)
      F->removeFnAttr(llvm::Attribute::NoDuplicate);
    if (Mutation == 6)
      F->removeFnAttr(llvm::Attribute::NoUnwind);
    if (Mutation == 7)
      Call->setCallingConv(llvm::CallingConv::Fast);
    if (Mutation == 8)
      llvm::BasicBlock::Create(C, "definition", F);
    if (Mutation == 9)
      Call->setArgOperand(0, B.getInt32(0));
    if (Mutation == 10)
      Call->setArgOperand(3, Owner->getArg(0));
    if (Mutation == 11)
      Call->setArgOperand(4, B.getInt32(3));
    if (Mutation == 13)
      Call->setArgOperand(0, llvm::ConstantPointerNull::get(B.getPtrTy(1)));
    if (Mutation == 15)
      F->setCallingConv(llvm::CallingConv::Fast);
    if (Mutation == 16)
      F->addFnAttr(llvm::Attribute::NoSync);
    if (Mutation == 17)
      F->addFnAttr(llvm::Attribute::Speculatable);
    const std::vector<std::string> Args{"mem", "gprs", "flags", "768", "8"};
    if (Mutation) {
      EXPECT_THROW(
          renderX86HleNativeCall(Mutation == 14 ? Arch::X86 : Arch::X64, *Call,
                                 false, Args),
          std::invalid_argument);
    } else {
      const auto Text = renderX86HleNativeCall(Arch::X64, *Call, false, Args);
      EXPECT_NE(Text.find("__builtin_ia32_hle_context"), std::string::npos);
      if (!F->getIntrinsicID())
        EXPECT_FALSE(
            Codegen().compile(M, Arch::X64, BinaryFormat::ELF).Success);
    }
  }
}
