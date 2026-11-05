//===- X86BoundSourceTests.inc - Native BOUND source contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both source routes and object formats retain the native faulting operand.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86Bound.h"

namespace {
BinaryImage boundFunction(BinaryFormat Format, unsigned Width, unsigned Bits,
                          unsigned Prefix) {
  // cdecl index at SP+4 and bounds offset at SP+8; preserve the caller's DI.
  std::vector<uint8_t> B{0x57, 0x8b, 0x44, 0x24, 8, 0x8b, 0x7c, 0x24, 12};
  if (Width == 2)
    B.push_back(0x66);
  if (Bits == 16)
    B.push_back(0x67);
  if (Prefix)
    B.push_back(Prefix);
  B.insert(B.end(), {0x62, uint8_t(Bits == 16 ? 5 : 7), 0x5f, 0xc3});
  return image(Arch::X86, Format, B);
}
} // namespace

TEST(X86BoundSource, WidthsAddressSizesSegmentsAndFormatsRetainNativeAccess) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool NoOpt : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned Bits : {16u, 32u})
          for (unsigned Prefix :
               {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u}) {
            const X86BoundNative P{Width, Bits, Prefix};
            SCOPED_TRACE(testing::Message()
                         << unsigned(Format) << " " << NoOpt << " " << Width
                         << " " << Bits << " " << Prefix);
            auto Image = boundFunction(Format, Width, Bits, Prefix);
            std::string High, C, Native;
            std::vector<uint8_t> Object;
            ASSERT_NO_THROW(sourceRoutes(Image, NoOpt, Entry, {}, &High, &C, {},
                                         {}, &Native, &Object));
            ASSERT_FALSE(testing::Test::HasFailure());
            for (const auto &Source : {High, C})
              EXPECT_NE(Source.find("NEVERD_X86_BOUND(" + P.builtin()),
                        std::string::npos)
                  << Source;
            if (llvm::Intrinsic::lookupIntrinsicID(P.llvmName())) {
              EXPECT_NE(Native.find(P.llvmName()), std::string::npos) << Native;
              EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
            } else {
              EXPECT_NE(Native.find(P.assembly()), std::string::npos) << Native;
            }
            ASSERT_FALSE(Object.empty());
            const auto Name =
                "bound_" + std::to_string(unsigned(Format)) + "_" +
                std::to_string(NoOpt) + "_" + std::to_string(Width) + "_" +
                std::to_string(Bits) + "_" + std::to_string(Prefix);
            saveX86SourceArtifact(Name + ".high.c", High);
            saveX86SourceArtifact(Name + ".llvm.c", C);
            saveX86SourceArtifact(Name + ".ll", Native);
            saveX86SourceArtifact(
                Name + ".o",
                {reinterpret_cast<const char *>(Object.data()), Object.size()});
          }
}

TEST(X86BoundSource, HighContractRejectsMissingWidthsAndInventedEffects) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    HighExpr E;
    E.Kind = ExprKind::Call;
    E.IntrinsicId = Intrinsic::Bound;
    E.Type = NdType::makeVoid();
    const unsigned Sizes[]{2, 8, 4, 4};
    const uint64_t Values[]{1, 0x2000, 16, 0x64};
    for (unsigned I = 0; I != 4; ++I) {
      auto V = std::make_shared<HighExpr>();
      V->Kind = ExprKind::Const;
      V->Type = NdType::makeInt(Sizes[I], false);
      V->ConstVal = Values[I];
      E.Operands.push_back(V);
    }
    if (Mutation == 1)
      E.Operands.pop_back();
    if (Mutation == 2)
      E.Operands[0]->Type = NdType::makeInt(8, false);
    if (Mutation == 3)
      E.Operands[2]->ConstVal = 64;
    if (Mutation == 4)
      E.Operands[3]->ConstVal = 0x66;
    if (Mutation == 5)
      E.Type = NdType::makeInt(4, false);
    if (Mutation == 6)
      E.DoesNotReturn = true;
    if (Mutation == 7)
      E.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    if (Mutation == 8)
      E.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    if (Mutation == 9)
      E.Operands[1]->Type = NdType::makeFloat(8);
    if (Mutation == 12)
      E.Kind = ExprKind::Var;
    const auto Render = [&] {
      return x86BoundHighPlan(Mutation == 10   ? Arch::X64
                              : Mutation == 11 ? Arch::Unknown
                                               : Arch::X86,
                              E);
    };
    if (Mutation)
      EXPECT_THROW(Render(), std::invalid_argument);
    else
      EXPECT_EQ(Render().AddressBits, 16u);
  }
}

TEST(X86BoundSource, NarrowReadPreservesFullReturnValue) {
  for (bool NoOpt : {false, true}) {
    auto Image = boundFunction(BinaryFormat::ELF, 2, 32, 0);
    PipelineOptions O;
    O.EmitDumpOutput = false;
    O.NoOpt = NoOpt;
    O.OnlyFunctionEntries = {Entry};
    llvm::LLVMContext Context;
    auto R = Pipeline().run(Image, Context, O);
    ASSERT_TRUE(R.Success) << R.Error;
    std::string Dump;
    llvm::raw_string_ostream OS(Dump);
    Pipeline::dumpMedIR(R.MedFuncs, OS);
    Pipeline::dumpHighIR(R.HighFuncs, OS);
    SCOPED_TRACE(Dump);
    ASSERT_EQ(R.HighFuncs.size(), 1u);
    const auto &Body = R.HighFuncs.front().Body;
    ASSERT_FALSE(Body.empty());
    ASSERT_EQ(Body.back().Kind, StmtKind::Return);
    ASSERT_TRUE(Body.back().RetVal);
    ASSERT_TRUE(Body.back().RetVal->Type);
    EXPECT_EQ(Body.back().RetVal->Type->Size, 4u);
  }
}

TEST(X86BoundSource, MedContractRejectsUnknownControlsAndEffects) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    MedOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.addInput(MedVar::makeConst(unsigned(Intrinsic::Bound), 2));
    Op.addInput(MedVar::makeConst(1, 2));
    Op.addInput(MedVar::makeConst(0x1000, 8));
    Op.addInput(MedVar::makeConst(16, 4));
    Op.addInput(MedVar::makeConst(0x64, 4));
    if (Mutation == 1)
      Op.NumInputs = 4;
    if (Mutation == 2)
      Op.Inputs[0].Size = 4;
    if (Mutation == 3)
      Op.Inputs[1].Size = 8;
    if (Mutation == 4)
      Op.Inputs[2].Size = 4;
    if (Mutation == 5)
      Op.Inputs[3].ConstVal = 64;
    if (Mutation == 6)
      Op.Inputs[4].ConstVal = 0x66;
    if (Mutation == 7)
      Op.Output.Size = 4;
    if (Mutation == 8)
      Op.Inputs[3].Kind = MedVar::Param;
    if (Mutation == 9)
      Op.Inputs[2].Kind = MedVar::Unspecified;
    if (Mutation == 10)
      Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    if (Mutation == 11)
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    EXPECT_EQ(
        x86BoundMedShapeIsValid(Op, Mutation == 12 ? Arch::X64 : Arch::X86),
        Mutation == 0);
  }
}

TEST(X86BoundSource, LLVMContractRejectsChangedTypesModesAndEffects) {
  for (bool Native : {false, true}) {
    const X86BoundNative P{2, 16, 0x64};
    if (Native && !llvm::Intrinsic::lookupIntrinsicID(P.llvmName()))
      continue;
    for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
      // LLVM rejects these assembly types at construction, before rendering.
      if (!Native && (Mutation == 6 || Mutation == 7))
        continue;
      SCOPED_TRACE(testing::Message() << Native << " " << Mutation);
      llvm::LLVMContext Context;
      llvm::Module Module("bound-contract", Context);
      auto *Word = llvm::Type::getInt16Ty(Context);
      auto *Dword = llvm::Type::getInt32Ty(Context);
      auto *Void = llvm::Type::getVoidTy(Context);
      std::vector<llvm::Type *> Types{Mutation == 5 ? Dword : Word, Dword};
      if (Native) {
        Types.push_back(Dword);
        Types.push_back(Dword);
      }
      auto *Type = llvm::FunctionType::get(Mutation == 6 ? Dword : Void, Types,
                                           Mutation == 7);
      llvm::Value *Callee;
      if (Native)
        Callee = llvm::Function::Create(
            Type, llvm::GlobalValue::ExternalLinkage, P.llvmName(), Module);
      else
        Callee =
            llvm::InlineAsm::get(Type, P.assembly(), P.constraints(), true);
      std::vector<llvm::Value *> Args;
      for (auto *T : Types)
        Args.push_back(llvm::ConstantInt::get(T, 1));
      if (Native) {
        Args[2] = llvm::ConstantInt::get(Dword, 16);
        Args[3] = llvm::ConstantInt::get(Dword, 0x64);
      }
      auto *F = llvm::Function::Create(llvm::FunctionType::get(Void, false),
                                       llvm::GlobalValue::ExternalLinkage,
                                       "probe", Module);
      auto *B = llvm::BasicBlock::Create(Context, "entry", F);
      auto *Call = llvm::CallInst::Create(Type, Callee, Args, "", B);
      llvm::ReturnInst::Create(Context, B);
      if (Mutation == 1)
        Call->setMemoryEffects(llvm::MemoryEffects::readOnly());
      if (Mutation == 2)
        Call->setMemoryEffects(llvm::MemoryEffects::writeOnly());
      if (Mutation == 3)
        Call->addFnAttr(llvm::Attribute::NoReturn);
      if (Mutation == 4)
        Call->addFnAttr(llvm::Attribute::Speculatable);
      if (Mutation == 8)
        Call->setCallingConv(llvm::CallingConv::Fast);
      if (Mutation == 10)
        Call->setMemoryEffects(
            llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef));
      if (Mutation == 11)
        Call->setMemoryEffects(
            llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::ModRef));
      auto Plan = [&] {
        return x86BoundCallPlan(Mutation == 9    ? Arch::X64
                                : Mutation == 12 ? Arch::Unknown
                                                 : Arch::X86,
                                *Call);
      };
      if (Mutation)
        EXPECT_THROW(Plan(), std::invalid_argument);
      else
        ASSERT_TRUE(Plan());
    }
  }
}
