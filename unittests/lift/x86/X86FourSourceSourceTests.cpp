//===- X86FourSourceSourceTests.cpp - Four-source boundaries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate native lowering, source projection and raw decoder ownership.
//===----------------------------------------------------------------------===//
#include "../../../lib/backend/c/render/X86/X86FourSource.h"
#include "gtest/gtest.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/X86/X86FourSourceShape.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdlib>
#include <stdexcept>
using namespace neverd;
namespace {
std::vector<X86FourSourcePlan> plans(Arch A = Arch::X64) {
  std::vector<X86FourSourcePlan> R;
  for (unsigned Kind = 0; Kind < 6; ++Kind)
    for (unsigned Prefix :
         {0u, 0x64u, 0x65u, X86NativeStackReference, 0x26u, 0x2eu, 0x36u})
      if ((A != Arch::X86 || !x86NativeMemoryIsStack(Prefix)) &&
          (A == Arch::X86 || Prefix == 0 || Prefix >= 0x64))
        for (unsigned Zero : {0u, 1u})
          R.push_back(*x86FourSourcePlan(Kind, Zero, Prefix));
  return R;
}
unsigned control(const X86FourSourcePlan &P) {
  return static_cast<unsigned>(P.Control.Kind) |
         (P.Control.Zero ? X86FourSourceZero : 0) |
         (P.stackReference() ? X86FourSourceStackReference : 0) |
         ((P.Prefix == 0x26 || P.Prefix == 0x2e || P.Prefix == 0x36)
              ? P.Prefix << X86FourSourceSegmentShift
              : 0);
}
NdMemoryAddressSpace space(const X86FourSourcePlan &P) {
  return P.Prefix == 0x64   ? NdMemoryAddressSpace::X86FS
         : P.Prefix == 0x65 ? NdMemoryAddressSpace::X86GS
                            : NdMemoryAddressSpace::Default;
}
MedVar temporary(unsigned Id, unsigned Bytes) {
  MedVar V;
  V.Id = Id;
  V.Size = Bytes;
  V.TheArch = Arch::X64;
  return V;
}
MedFunc med(const X86FourSourcePlan &P) {
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = 0x1000;
  Op.MemoryAddressSpace = space(P);
  Op.Output = temporary(4, P.Control.bytes());
  Op.addInput(MedVar::makeConst(unsigned(Intrinsic::X86FourSourceMemory), 2));
  Op.addInput(MedVar::makeConst(0x12345678, 8));
  Op.addInput(MedVar::makeConst(0x87654321, P.Control.bytes()));
  Op.addInput(MedVar::makeConst(0xabcdef01, P.Control.sourceBytes() * 4));
  Op.addInput(MedVar::makeConst(0x55, P.Control.maskBytes()));
  Op.addInput(MedVar::makeConst(control(P), control(P) > UINT8_MAX ? 2 : 1));
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  MedBlock B;
  B.Id = 0;
  B.StartAddr = 0x1000;
  B.EndAddr = 0x1001;
  B.Ops = {Op, Ret};
  MedFunc F;
  F.Name = "four_med";
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeVoid();
  F.Blocks.push_back(B);
  return F;
}
ExprPtr high(const X86FourSourcePlan &P) {
  auto E = HighExpr::makeCall("four", 0, {});
  E->IntrinsicId = Intrinsic::X86FourSourceMemory;
  E->MemoryAddressSpace = space(P);
  E->Type = NdType::makeInt(P.Control.bytes(), false);
  E->Operands = {
      HighExpr::makeVar(temporary(0, 8)),
      HighExpr::makeVar(temporary(1, P.Control.bytes())),
      HighExpr::makeVar(temporary(2, P.Control.sourceBytes() * 4)),
      HighExpr::makeVar(temporary(3, P.Control.maskBytes())),
      HighExpr::makeConst(control(P), control(P) > UINT8_MAX ? 2 : 1)};
  return E;
}
void write(const std::string &Name, llvm::StringRef Text) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Name, EC);
  ASSERT_FALSE(EC);
  OS << Text;
}
llvm::CallInst *call(llvm::IRBuilder<> &B, llvm::Module &M,
                     const X86FourSourcePlan &P,
                     std::vector<llvm::Value *> Args, bool Native) {
  if (Native) {
    Args.push_back(B.getInt32(P.Control.Zero));
    Args.push_back(B.getInt32(P.Prefix));
    return B.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(
            &M, llvm::Intrinsic::lookupIntrinsicID(P.llvmName())),
        Args);
  }
  std::vector<llvm::Type *> Types;
  for (const auto *V : Args)
    Types.push_back(V->getType());
  auto *T = llvm::FunctionType::get(Args[1]->getType(), Types, false);
  return B.CreateCall(
      llvm::InlineAsm::get(T, P.assembly(), P.constraints(), true), Args);
}

TEST(X86FourSourceSource, StackControlsRequire64BitDefaultMemory) {
  for (unsigned Control : {0x164u, 0x165u, 0x200u})
    EXPECT_FALSE(x86FourSourcePlan(0, 0, Control));
  for (const auto &P : plans()) {
    if (!P.stackReference())
      continue;
    auto F = med(P);
    auto E = high(P);
    EXPECT_THROW(x86FourSourceHighPlan(Arch::X86, *E), std::invalid_argument);
    llvm::LLVMContext C;
    EXPECT_THROW(MedLLVMEmitter().emit({F}, C, "wrong-mode", Arch::X86),
                 std::invalid_argument);
    for (auto Space :
         {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS}) {
      F.Blocks[0].Ops[0].MemoryAddressSpace = Space;
      E->MemoryAddressSpace = Space;
      EXPECT_THROW(x86FourSourceHighPlan(Arch::X64, *E), std::invalid_argument);
      EXPECT_THROW(MedLLVMEmitter().emit({F}, C, "wrong-segment", Arch::X64),
                   std::invalid_argument);
    }
  }
}

TEST(X86FourSourceSource, MedAndSourceShareCheckedControls) {
  for (const auto &P : plans()) {
    EXPECT_TRUE(intrinsicMemoryAddressSpaceShapeIsValid(
        Intrinsic::X86FourSourceMemory, 6, P.Control.bytes(), 8,
        P.Control.bytes(), X86FourSourceCount * P.Control.sourceBytes()));
    llvm::LLVMContext C;
    auto M = MedLLVMEmitter().emit({med(P)}, C, "four", Arch::X64);
    ASSERT_TRUE(M);
    ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
    unsigned Count = 0;
    for (auto &F : *M)
      for (auto &B : F)
        for (auto &I : B)
          if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I))
            if (auto Actual = x86FourSourceCallPlan(Arch::X64, *Call)) {
              ++Count;
              EXPECT_EQ(Actual->Control.Kind, P.Control.Kind);
              EXPECT_EQ(Actual->Control.Zero, P.Control.Zero);
              EXPECT_EQ(Actual->Prefix, P.Prefix);
            }
    EXPECT_EQ(Count, 1u);
    EXPECT_EQ(x86FourSourceHighPlan(Arch::X64, *high(P)).llvmName(),
              P.llvmName());
  }
  const auto P = *x86FourSourcePlan(0, 0, 0);
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    auto F = med(P);
    auto &Op = F.Blocks[0].Ops[0];
    auto E = high(P);
    switch (Mutation) {
    case 0:
      Op.Inputs[5].ConstVal = 6;
      E->Operands[4]->ConstVal = 6;
      break;
    case 1:
      Op.Inputs[5].ConstVal = 0x100;
      E->Operands[4]->ConstVal = 0x100;
      break;
    case 2:
      Op.Inputs[3].Size = 64;
      E->Operands[2]->Type = NdType::makeInt(64, false);
      break;
    case 3:
      Op.Inputs[4].Size = 1;
      E->Operands[3]->Type = NdType::makeInt(1, false);
      break;
    case 4:
      Op.Inputs[1].Size = 4;
      E->Operands[0]->Type = NdType::makeInt(4, false);
      break;
    case 5:
      Op.Output.Size = 16;
      E->Type = NdType::makeInt(16, false);
      break;
    case 6:
      Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
      E->MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case 7:
      Op.Inputs[5] = temporary(99, 1);
      E->Operands[4] = HighExpr::makeVar(temporary(99, 1));
      break;
    }
    EXPECT_THROW(x86FourSourceHighPlan(Arch::X64, *E), std::invalid_argument);
    llvm::LLVMContext C;
    EXPECT_THROW(MedLLVMEmitter().emit({F}, C, "invalid", Arch::X64),
                 std::invalid_argument);
  }
  for (unsigned Count = 0; Count < 6; ++Count) {
    auto F = med(P);
    F.Blocks[0].Ops[0].NumInputs = Count;
    EXPECT_FALSE(
        x86FourSourceShapeIsValid(x86FourSourceMedShape(F.Blocks[0].Ops[0])));
    F = med(P);
    F.Blocks[0].Ops[0].Inputs.resize(Count);
    EXPECT_FALSE(
        x86FourSourceShapeIsValid(x86FourSourceMedShape(F.Blocks[0].Ops[0])));
  }
}

TEST(X86FourSourceSource, UnknownInputsAndSuppressionRetainArchitecturalUses) {
  for (const auto &P : plans()) {
    for (unsigned Input : {1u, 2u, 3u, 4u}) {
      auto F = med(P);
      auto &V = F.Blocks[0].Ops[0].Inputs[Input];
      V = temporary(99, V.Size);
      llvm::LLVMContext C;
      EXPECT_THROW(MedLLVMEmitter().emit({F}, C, "unknown", Arch::X64),
                   std::invalid_argument);
    }
    auto F = med(P);
    auto &Op = F.Blocks[0].Ops[0];
    Op.Inputs[4] = MedVar::makeConst(0, P.Control.maskBytes());
    Op.Inputs[1] = temporary(91, 8);
    Op.Inputs[3] = temporary(93, P.Control.sourceBytes() * 4);
    llvm::LLVMContext C;
    EXPECT_NO_THROW(MedLLVMEmitter().emit({F}, C, "suppressed", Arch::X64));
    Op.Inputs[2] = temporary(92, P.Control.bytes());
    if (P.Control.Zero && !P.Control.scalar())
      EXPECT_NO_THROW(MedLLVMEmitter().emit({F}, C, "zero", Arch::X64));
    else
      EXPECT_THROW(MedLLVMEmitter().emit({F}, C, "old", Arch::X64),
                   std::invalid_argument);
    auto E = high(P);
    E->Operands[3] = HighExpr::makeConst(0, P.Control.maskBytes());
    const auto Type =
        "unsigned _BitInt(" + std::to_string(P.Control.bytes() * 8) + ")";
    auto Text = renderX86FourSourceExpression(
        Arch::X64, *E,
        {"unknown_address", "prior", "unknown_sources", "0", "control"}, Type);
    EXPECT_EQ(Text.find("unknown_address"), std::string::npos);
    EXPECT_EQ(Text.find("unknown_sources"), std::string::npos);
    EXPECT_EQ(Text.find("prior") != std::string::npos,
              !P.Control.Zero || P.Control.scalar());
    E->Operands[2] = HighExpr::makeCall("produce", 0, {});
    E->Operands[2]->Type = NdType::makeInt(P.Control.sourceBytes() * 4, false);
    Text = renderX86FourSourceExpression(
        Arch::X64, *E,
        {"unknown_address", "prior", "produce()", "0", "control"}, Type);
    EXPECT_NE(Text.find("produce()"), std::string::npos);
    EXPECT_EQ(Text.find("unknown_address"), std::string::npos);
  }
}

TEST(X86FourSourceSource, OwnedCallsRejectChangedEffectsAndRegisterGroups) {
  for (unsigned Kind : {0u, 2u, 4u}) {
    const auto P = *x86FourSourcePlan(Kind, 0, 0);
    for (bool Native : {false, true}) {
      if (Native && !llvm::Intrinsic::lookupIntrinsicID(P.llvmName()))
        continue;
      for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
        SCOPED_TRACE(Kind);
        SCOPED_TRACE(Native);
        SCOPED_TRACE(Mutation);
        llvm::LLVMContext C;
        llvm::Module M("invalid-four", C);
        llvm::IRBuilder<> B(C);
        auto *F = llvm::Function::Create(
            llvm::FunctionType::get(B.getVoidTy(), false),
            llvm::GlobalValue::ExternalLinkage, "bad", M);
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
        auto *V = llvm::Constant::getNullValue(
            llvm::FixedVectorType::get(B.getInt32Ty(), P.Control.bytes() / 4));
        auto *I = call(B, M, P,
                       {llvm::ConstantPointerNull::get(B.getPtrTy()), V, V, V,
                        V, V, B.getInt16(0)},
                       Native);
        B.CreateRetVoid();
        Arch A = Arch::X64;
        switch (Mutation) {
        case 0:
          A = Arch::AArch64;
          break;
        case 1:
          I->setMemoryEffects(llvm::MemoryEffects::none());
          break;
        case 2:
          I->addFnAttr(llvm::Attribute::NoSync);
          break;
        case 3:
          I->addFnAttr(llvm::Attribute::NoReturn);
          break;
        case 4:
          I->setCallingConv(llvm::CallingConv::Fast);
          break;
        default:
          if (Native) {
            if (Mutation == 5)
              I->setArgOperand(7, B.getInt32(2));
            else if (Mutation == 6)
              I->setArgOperand(8, B.getInt32(1));
            else
              I->setArgOperand(8, llvm::UndefValue::get(B.getInt32Ty()));
          } else {
            const auto *Asm =
                llvm::cast<llvm::InlineAsm>(I->getCalledOperand());
            std::string Constraints =
                llvm::StringRef(Asm->getConstraintString()).str();
            if (Mutation == 6) {
              const auto Register = P.sourceRegister(3);
              Constraints.replace(Constraints.find(Register), Register.size(),
                                  P.sourceRegister(2));
            }
            I->setCalledOperand(llvm::InlineAsm::get(
                Asm->getFunctionType(), Asm->getAsmString(), Constraints,
                Mutation != 5, false,
                Mutation == 7 ? llvm::InlineAsm::AD_Intel
                              : llvm::InlineAsm::AD_ATT));
          }
        }
        EXPECT_THROW(x86FourSourceCallPlan(A, *I), std::invalid_argument);
        CEmitterOptions O;
        O.TheArch = A;
        std::string Source;
        llvm::raw_string_ostream OS(Source);
        EXPECT_THROW(LLVMCEmitter().emit(M, OS, O), std::invalid_argument);
        EXPECT_TRUE(Source.empty());
      }
    }
  }
}

TEST(X86FourSourceSource, ExportsEverySourceAndMaskedEffect) {
  const auto *Dir = std::getenv("NEVERD_FOUR_SOURCE_EXPORT_DIR");
  if (Dir)
    ASSERT_FALSE(llvm::sys::fs::create_directories(Dir));
  for (Arch A : {Arch::X64, Arch::X86}) {
    llvm::LLVMContext C;
    llvm::Module M("four-wrappers", C);
    M.setTargetTriple(llvm::Triple(A == Arch::X64 ? "x86_64-unknown-linux-gnu"
                                                  : "i386-unknown-linux-gnu"));
    std::string High =
        "#include <stdint.h>\n" + std::string(X86FourSourceCompilerDispatch);
    unsigned Index = 0;
    for (const auto &P : plans(A)) {
      const auto Bytes = P.Control.bytes();
      const auto Name = "four_" + std::to_string(Index++);
      llvm::IRBuilder<> B(C);
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(
              B.getVoidTy(),
              {B.getPtrTy(), B.getInt16Ty(), B.getPtrTy(), B.getPtrTy()},
              false),
          llvm::GlobalValue::ExternalLinkage, Name, M);
      std::string Features;
      for (const auto &Feature : P.features())
        Features += (Features.empty() ? "+" : ",+") + Feature;
      F->addFnAttr("target-features", Features);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
      auto *V = llvm::FixedVectorType::get(B.getInt32Ty(), Bytes / 4);
      std::vector<llvm::Value *> Args = {F->getArg(0)};
      for (unsigned I = 0; I < 5; ++I)
        Args.push_back(B.CreateAlignedLoad(
            V, B.CreateGEP(B.getInt8Ty(), F->getArg(2), B.getInt32(I * Bytes)),
            llvm::Align(1)));
      Args.push_back(F->getArg(1));
      const bool Native =
          llvm::Intrinsic::lookupIntrinsicID(P.llvmName()) &&
          (!P.stackReference() || x86NativeStackMemoryIsAvailable());
      B.CreateAlignedStore(call(B, M, P, Args, Native), F->getArg(3),
                           llvm::Align(1));
      Args.back() = B.getInt16(0);
      call(B, M, P, Args, Native);
      B.CreateRetVoid();
      const auto Type = "unsigned _BitInt(" + std::to_string(Bytes * 8) + ")";
      High += "__attribute__((target(\"avx512f\"))) void " + Name +
              "(void *address, uint16_t mask, void *input, void *output) { " +
              Type + " prior; unsigned _BitInt(" +
              std::to_string(P.Control.sourceBytes() * 4 * 8) +
              ") sources; __builtin_memcpy(&prior,input," +
              std::to_string(Bytes) + "); ";
      for (unsigned I = 0; I < 4; ++I)
        High += "__builtin_memcpy((char *)&sources+" +
                std::to_string(I * P.Control.sourceBytes()) +
                ",(char *)input+" + std::to_string((I + 1) * Bytes) + "," +
                std::to_string(P.Control.sourceBytes()) + "); ";
      auto E = high(P);
      High +=
          Type + " result = " +
          renderX86FourSourceExpression(
              A, *E, {"address", "prior", "sources", "mask", "control"}, Type) +
          "; __builtin_memcpy(output,&result," + std::to_string(Bytes) + "); ";
      E->Operands[3] = HighExpr::makeConst(0, P.Control.maskBytes());
      High +=
          "(void)" +
          renderX86FourSourceExpression(
              A, *E, {"address", "prior", "sources", "0", "control"}, Type) +
          "; }\n";
    }
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    CEmitterOptions O;
    O.TheArch = A;
    O.Format = BinaryFormat::ELF;
    std::string Source, IR;
    llvm::raw_string_ostream OS(Source), IROS(IR);
    ASSERT_TRUE(LLVMCEmitter().emit(M, OS, O));
    M.print(IROS, nullptr);
    const auto Stem =
        std::string(Dir ? Dir : "") + (A == Arch::X64 ? "/x64" : "/x86");
    if (Dir) {
      write(Stem + ".ll", IR);
      write(Stem + "-high.c", High);
      write(Stem + "-llvm.c", Source);
    }
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      auto Object = Codegen().compile(M, A, Format);
      ASSERT_TRUE(Object.Success);
      if (Dir)
        write(Stem + "-" + std::to_string(unsigned(Format)) + ".o",
              llvm::StringRef(
                  reinterpret_cast<const char *>(Object.ObjectData.data()),
                  Object.ObjectData.size()));
    }
  }
}

TEST(X86FourSourceSource, PipelineKeepsAllFourInputsAndTheReturnedValue) {
  const auto *Dir = std::getenv("NEVERD_FOUR_SOURCE_EXPORT_DIR");
  if (Dir)
    ASSERT_FALSE(
        llvm::sys::fs::create_directories(std::string(Dir) + "/pipeline"));
  unsigned Index = 0;
  for (const auto &P : plans()) {
    // Address, mask, accumulator plus four sources, and output are independent
    // SysV arguments. Every source has a different observable input slice.
    std::vector<uint8_t> Bytes = {0x48, 0x89, 0xf8, 0xc5, 0xf8, 0x92, 0xd6};
    if (P.stackReference()) {
      // Preserve RBP and retain the known RAX return of the ordinary fixture.
      Bytes.insert(Bytes.begin(), {0x55, 0x48, 0x89, 0xfd});
    }
    const auto Load = P.Control.scalar()
                          ? std::vector<uint8_t>{0xc5, 0xfa, 0x6f}
                          : std::vector<uint8_t>{0x62, 0xf1, 0xfe, 0x48, 0x6f};
    for (unsigned I = 0; I < 5; ++I) {
      Bytes.insert(Bytes.end(), Load.begin(), Load.end());
      const unsigned Reg = I ? I + 3 : 1;
      Bytes.push_back((I ? 0x40 : 0) | (Reg << 3) | 2);
      if (I)
        Bytes.push_back(P.Control.scalar() ? I * 16 : I);
    }
    if (P.hasSegmentPrefix())
      Bytes.push_back(x86NativeMemoryPrefix(P.Prefix));
    constexpr uint8_t Opcodes[] = {0x9a, 0xaa, 0x9b, 0xab, 0x52, 0x53};
    Bytes.insert(Bytes.end(), {0x62, 0xf2, 0x5f,
                               uint8_t(0x0a | (P.Control.scalar() ? 0 : 0x40) |
                                       (P.Control.Zero ? 0x80 : 0)),
                               Opcodes[static_cast<unsigned>(P.Control.Kind)],
                               uint8_t(P.stackReference() ? 0x4d : 0x08)});
    if (P.stackReference())
      Bytes.push_back(0); // RBP + disp8(0).
    auto Store = Load;
    Store.back() = 0x7f;
    Bytes.insert(Bytes.end(), Store.begin(), Store.end());
    Bytes.push_back(0x09);
    if (P.stackReference())
      Bytes.push_back(0x5d); // pop rbp.
    Bytes.push_back(0xc3);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.Entry = Image.Base = 0x1000;
    Segment S;
    S.VA = 0x1000;
    S.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    S.Data = Bytes;
    S.Size = S.FileSz = Bytes.size();
    Image.Segments.push_back(S);
    auto Symbol = Symbol::makeFunc(0x1000, Bytes.size());
    Symbol.Name = "four_pipeline";
    Image.Symbols.push_back(Symbol);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {0x1000};
    llvm::LLVMContext C;
    auto R = Pipeline().run(Image, C, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    auto M = MedLLVMEmitter().emit(R.MedFuncs, C, "four", Arch::X64, {}, &Image,
                                   BinaryFormat::ELF);
    ASSERT_TRUE(M);
    ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
    CEmitterOptions O;
    O.TheArch = Arch::X64;
    O.Format = BinaryFormat::ELF;
    O.Image = &Image;
    std::string H, L, IR;
    llvm::raw_string_ostream HOS(H), LOS(L), IROS(IR);
    ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, HOS, O));
    Pipeline::promoteScaffoldingAllocas(*M);
    ASSERT_TRUE(LLVMCEmitter().emit(*M, LOS, O));
    M->print(IROS, nullptr);
    EXPECT_NE(H.find(P.builtin()), std::string::npos) << H;
    EXPECT_NE(L.find(P.builtin()), std::string::npos) << L;
    EXPECT_EQ(H.find("unknown register"), std::string::npos) << H;
    EXPECT_EQ(L.find("unknown register"), std::string::npos) << L;
    EXPECT_EQ(H.find("segmented-memory output requires Clang"),
              std::string::npos)
        << H;
    auto Object = Codegen().compile(*M, Arch::X64, BinaryFormat::ELF);
    ASSERT_TRUE(Object.Success);
    if (Dir) {
      const auto Stem = std::string(Dir) + "/pipeline/" + std::to_string(Index);
      write(Stem + "-high.c", H);
      write(Stem + "-llvm.c", L);
      write(Stem + ".ll", IR);
      write(Stem + ".o", llvm::StringRef(reinterpret_cast<const char *>(
                                             Object.ObjectData.data()),
                                         Object.ObjectData.size()));
    }
    ++Index;
  }
  EXPECT_EQ(Index, 48u);
}
} // namespace
