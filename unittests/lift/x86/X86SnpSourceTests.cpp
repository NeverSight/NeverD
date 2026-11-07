//===- X86SnpSourceTests.cpp - SNP native completion boundaries ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject erased memory effects, truncated contexts and unbound encodings.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86Virtualization.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86SnpEncoding.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

TEST(X86SnpSource, NativeCallsRetainExactEffectsAndCompletionObjects) {
  for (const char *Name :
       {"psmash.context", "pvalidate.context", "rmpadjust.context",
        "rmpupdate.context", "rmpquery.context", "rmpread.context",
        "rmpopt.context", "rmpchkd.context", "vmgexit.sev"})
    for (unsigned Mutation = 0; Mutation != 16; ++Mutation) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(Mutation);
      const bool Context = llvm::StringRef(Name) != "vmgexit.sev";
      llvm::LLVMContext C;
      llvm::Module M("snp", C);
      llvm::IRBuilder<> B(C);
      std::vector<llvm::Type *> Types =
          Context ? std::vector<llvm::Type *>{B.getPtrTy(), B.getPtrTy(),
                                              B.getInt32Ty()}
                  : std::vector<llvm::Type *>{B.getInt64Ty()};
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), Types, Mutation == 13),
          llvm::GlobalValue::ExternalLinkage, std::string("llvm.x86.") + Name,
          M);
      for (auto Attr :
           {llvm::Attribute::Convergent, llvm::Attribute::NoDuplicate,
            llvm::Attribute::NoUnwind})
        F->addFnAttr(Attr);
      auto *Owner = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), {B.getInt32Ty()}, false),
          llvm::GlobalValue::ExternalLinkage, "owner", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Owner));
      auto *P = llvm::ConstantPointerNull::get(B.getPtrTy());
      std::vector<llvm::Value *> Values =
          Context ? std::vector<llvm::Value *>{P, P, B.getInt32(0)}
                  : std::vector<llvm::Value *>{B.getInt64(71)};
      auto *Call = B.CreateCall(F, Values);
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
        Call->setArgOperand(0, B.getInt32(7));
      if (Mutation == 10)
        Call->setArgOperand(Context ? 2 : 0,
                            Context
                                ? static_cast<llvm::Value *>(Owner->getArg(0))
                                : B.getInt32(1));
      if (Mutation == 11)
        Call->setArgOperand(Context ? 2 : 0,
                            Context ? B.getInt32(2) : B.getInt32(1));
      if (Mutation == 12)
        Call->setArgOperand(0, llvm::ConstantPointerNull::get(B.getPtrTy(1)));
      if (Mutation == 15)
        F->setCallingConv(llvm::CallingConv::Fast);
      const std::vector<std::string> Arguments =
          Context ? std::vector<std::string>{"gprs", "flags", "0"}
                  : std::vector<std::string>{"parameter"};
      if (Mutation) {
        EXPECT_THROW(renderX86VirtualizationNativeCall(
                         Mutation == 14 ? Arch::X86 : Arch::X64, *Call, {},
                         false, Arguments),
                     std::invalid_argument);
        continue;
      }
      const auto Text = renderX86VirtualizationNativeCall(Arch::X64, *Call, {},
                                                          false, Arguments);
      ASSERT_TRUE(Text);
      EXPECT_NE(Text->find("__builtin_ia32_"), std::string::npos);
      EXPECT_EQ(Text->find("asm("), std::string::npos);
      if (!F->getIntrinsicID()) {
        const auto Result = Codegen().compile(M, Arch::X64, BinaryFormat::ELF);
        EXPECT_FALSE(Result.Success);
        EXPECT_TRUE(Result.ObjectData.empty());
      }
    }
}

TEST(X86SnpSource, ControlsAndContextsHaveOneShapeAuthority) {
  for (const auto Id : {Intrinsic::X86SnpPsmash, Intrinsic::X86SnpPvalidate,
                        Intrinsic::X86SnpRmpadjust, Intrinsic::X86SnpRmpupdate,
                        Intrinsic::X86SnpRmpquery, Intrinsic::X86SnpRmpread,
                        Intrinsic::X86SnpRmpopt, Intrinsic::X86SnpRmpchkd}) {
    ASSERT_TRUE(isSideeffectIntrinsic(Id));
    ASSERT_NE(intrinsicCName(Id), nullptr);
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.NumInputs = 4;
    Op.Inputs[0] = NdVar::cst(uint64_t(Id), 2);
    Op.Inputs[1] = NdVar::tmp(1, 8);
    Op.Inputs[2] = NdVar::tmp(2, 8);
    Op.Inputs[3] = NdVar::cst(0, 4);
    for (unsigned Segment : {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u})
      for (unsigned Width = 0; Width != 2; ++Width) {
        Op.Inputs[3].Offset = (Segment << 8) | Width;
        EXPECT_EQ(x86VirtualizationShapeIsValid(
                      Id, x86VirtualizationLowShape(Op, Arch::X64)),
                  !Width || Id == Intrinsic::X86SnpPvalidate);
      }
    Op.Inputs[3].Offset = 0;
    EXPECT_FALSE(x86VirtualizationShapeIsValid(
        Id, x86VirtualizationLowShape(Op, Arch::X86)));
    Op.Inputs[2].Size = 4;
    EXPECT_FALSE(x86VirtualizationShapeIsValid(
        Id, x86VirtualizationLowShape(Op, Arch::X64)));
    Op.Inputs[2].Size = 8;
    for (uint64_t Bad : {2ull, 0x6600ull, 0x10000ull, 0x100000000ull}) {
      Op.Inputs[3].Offset = Bad;
      EXPECT_FALSE(x86VirtualizationShapeIsValid(
          Id, x86VirtualizationLowShape(Op, Arch::X64)));
    }
  }
  EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86SevVmgexit));
}

TEST(X86SnpSource, RawAdmissionPreservesWidthsAndDoesNotInferAnEnvironment) {
  const std::vector<std::vector<uint8_t>> Encodings = {
      {0xf3, 0x0f, 0x01, 0xff}, {0xf2, 0x0f, 0x01, 0xff},
      {0xf3, 0x0f, 0x01, 0xfe}, {0xf2, 0x0f, 0x01, 0xfe},
      {0xf3, 0x0f, 0x01, 0xfd}, {0xf2, 0x0f, 0x01, 0xfd},
      {0xf2, 0x0f, 0x01, 0xfc}, {0xf3, 0x0f, 0x01, 0xfc},
      {0xf2, 0x0f, 0x01, 0xd9}, {0xf3, 0x0f, 0x01, 0xd9}};
  for (const auto &Bytes : Encodings) {
    const auto Encoding = x86SnpEncoding(Bytes);
    ASSERT_TRUE(Encoding);
    EXPECT_EQ(Encoding->Control, 0u);
    for (unsigned Size = 0; Size != Bytes.size(); ++Size)
      EXPECT_FALSE(x86SnpEncoding(llvm::ArrayRef(Bytes).take_front(Size)));
    for (uint8_t Prefix : {0x67, 0x64, 0x65, 0x66, 0xf0, 0x48}) {
      auto More = Bytes;
      More.insert(More.begin(), Prefix);
      const auto Result = x86SnpEncoding(More);
      EXPECT_EQ(Result.has_value(),
                Prefix == 0x67 && Encoding->Id == Intrinsic::X86SnpPvalidate);
      if (Result)
        EXPECT_EQ(Result->Control, 1u);
    }
  }
  EXPECT_FALSE(x86SnpEncoding({0x0f, 0x01, 0xd9}));
}

TEST(X86SnpSource, ContextsCannotAcquireAnIncompleteAssemblyFallback) {
  for (const auto Id : {Intrinsic::X86SnpPsmash, Intrinsic::X86SnpPvalidate,
                        Intrinsic::X86SnpRmpadjust, Intrinsic::X86SnpRmpupdate,
                        Intrinsic::X86SnpRmpquery, Intrinsic::X86SnpRmpread,
                        Intrinsic::X86SnpRmpopt, Intrinsic::X86SnpRmpchkd,
                        Intrinsic::X86SevVmgexit, Intrinsic::X86SvmRunContext,
                        Intrinsic::X86HyperVCall, Intrinsic::X86KvmVmcall,
                        Intrinsic::X86KvmVmmcall}) {
    EXPECT_FALSE(
        x86VirtualizationAsm(Id, Arch::X64, NdMemoryAddressSpace::Default));
    EXPECT_FALSE(
        x86VirtualizationNative(Id, Arch::X64, NdMemoryAddressSpace::Default));
  }
  EXPECT_TRUE(x86VirtualizationAsm(Intrinsic::VmxWrite, Arch::X64,
                                   NdMemoryAddressSpace::Default));
}
