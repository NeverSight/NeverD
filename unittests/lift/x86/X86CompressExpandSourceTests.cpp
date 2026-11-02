//===- X86CompressExpandSourceTests.cpp - Compact memory boundaries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate native lowering, source projection and raw decoder ownership.
//===----------------------------------------------------------------------===//
#include "../../../lib/backend/c/render/X86/X86CompressExpand.h"
#include "gtest/gtest.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/X86/X86CompactShape.h"
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
std::vector<X86CompactPlan> plans(Arch A = Arch::X64) {
  std::vector<X86CompactPlan> R;
  for (unsigned Bytes : {16u, 32u, 64u})
    for (unsigned Kind = 0; Kind < 6; ++Kind)
      for (unsigned Prefix :
           {0u, 0x64u, 0x65u, X86NativeStackReference, 0x26u, 0x2eu, 0x36u})
        if (A == Arch::X86 ? !x86NativeMemoryIsStack(Prefix)
                           : Prefix != 0x26 && Prefix != 0x2e && Prefix != 0x36)
          for (unsigned Mode = 0; Mode < 3; ++Mode)
            R.push_back(
                *x86CompactPlan(Mode != 0, Bytes, Kind, Mode == 2, Prefix));
  return R;
}
unsigned control(const X86CompactPlan &P) {
  return P.elementBytes() | (P.Kind >= 4 ? X86CompactFloat : 0) |
         (P.Zero ? X86CompactZero : 0) |
         (P.stackReference() ? X86CompactStackReference : 0) |
         ((P.Prefix == 0x26 || P.Prefix == 0x2e || P.Prefix == 0x36)
              ? P.Prefix << X86CompactSegmentShift
              : 0);
}
Intrinsic id(const X86CompactPlan &P) {
  return P.Expand ? Intrinsic::EVEXExpandLoad : Intrinsic::EVEXCompressStore;
}
NdMemoryAddressSpace space(const X86CompactPlan &P) {
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
MedFunc med(const X86CompactPlan &P) {
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = 0x1000;
  Op.MemoryAddressSpace = space(P);
  if (P.Expand)
    Op.Output = temporary(4, P.Bytes);
  Op.addInput(MedVar::makeConst(unsigned(id(P)), 2));
  Op.addInput(MedVar::makeConst(0x12345678, 8));
  Op.addInput(MedVar::makeConst(0x55, 8));
  Op.addInput(MedVar::makeConst(0x87654321, P.Bytes));
  Op.addInput(MedVar::makeConst(control(P), control(P) > UINT8_MAX ? 2 : 1));
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  MedBlock B;
  B.Id = 0;
  B.StartAddr = 0x1000;
  B.EndAddr = 0x1001;
  B.Ops = {Op, Ret};
  MedFunc F;
  F.Name = "compact_med";
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeVoid();
  F.Blocks.push_back(B);
  return F;
}
ExprPtr high(const X86CompactPlan &P) {
  auto E = HighExpr::makeCall("compact", 0, {});
  E->IntrinsicId = id(P);
  E->MemoryAddressSpace = space(P);
  E->Type = P.Expand ? NdType::makeInt(P.Bytes, false) : NdType::makeVoid();
  E->Operands = {
      HighExpr::makeVar(temporary(0, 8)), HighExpr::makeVar(temporary(1, 8)),
      HighExpr::makeVar(temporary(2, P.Bytes)),
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
                     const X86CompactPlan &P, llvm::Value *Address,
                     llvm::Value *Mask, llvm::Value *Data, bool Native) {
  if (Native) {
    std::vector<llvm::Value *> Args = {Address, Mask, Data, B.getInt32(P.Kind)};
    if (P.Expand)
      Args.push_back(B.getInt32(P.Zero));
    Args.push_back(B.getInt32(P.Prefix));
    return B.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(
            &M, llvm::Intrinsic::lookupIntrinsicID(P.llvmName())),
        Args);
  }
  auto *T = llvm::FunctionType::get(
      P.Expand ? Data->getType() : B.getVoidTy(),
      {B.getPtrTy(), B.getIntNTy(P.maskBits()), Data->getType()}, false);
  return B.CreateCall(
      llvm::InlineAsm::get(T, P.assembly(), P.constraints(), true),
      {Address, B.CreateZExtOrTrunc(Mask, B.getIntNTy(P.maskBits())), Data});
}
TEST(X86CompressExpandSource, StackControlsRequire64BitDefaultMemory) {
  for (unsigned Control : {0x164u, 0x165u, 0x200u})
    EXPECT_FALSE(x86CompactPlan(true, 16, 2, 0, Control));
  for (const auto &P : plans()) {
    if (!P.stackReference())
      continue;
    auto F = med(P);
    auto E = high(P);
    EXPECT_THROW(x86CompactHighPlan(Arch::X86, *E), std::invalid_argument);
    llvm::LLVMContext C;
    EXPECT_THROW(MedLLVMEmitter().emit({F}, C, "wrong-mode", Arch::X86),
                 std::invalid_argument);
    for (auto Space :
         {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS}) {
      F.Blocks[0].Ops[0].MemoryAddressSpace = Space;
      E->MemoryAddressSpace = Space;
      EXPECT_THROW(x86CompactHighPlan(Arch::X64, *E), std::invalid_argument);
      EXPECT_THROW(MedLLVMEmitter().emit({F}, C, "wrong-segment", Arch::X64),
                   std::invalid_argument);
    }
  }
}

TEST(X86CompressExpandSource, MedAndSourceShareCheckedControls) {
  for (const auto &P : plans()) {
    llvm::LLVMContext C;
    auto M = MedLLVMEmitter().emit({med(P)}, C, "compact", Arch::X64);
    ASSERT_TRUE(M);
    ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
    unsigned Count = 0;
    for (auto &F : *M)
      for (auto &B : F)
        for (auto &I : B)
          if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I))
            if (auto Actual = x86CompactCallPlan(Arch::X64, *Call)) {
              ++Count;
              EXPECT_EQ(Actual->Kind, P.Kind);
              EXPECT_EQ(Actual->Bytes, P.Bytes);
              EXPECT_EQ(Actual->Zero, P.Zero);
              EXPECT_EQ(Actual->Prefix, P.Prefix);
            }
    EXPECT_EQ(Count, 1u);
    const auto E = high(P);
    EXPECT_EQ(x86CompactHighPlan(Arch::X64, *E).mnemonic(), P.mnemonic());
    EXPECT_NE(renderX86CompactExpression(
                  Arch::X64, *E, {"address", "mask", "data", "control"},
                  "unsigned _BitInt(" + std::to_string(P.Bytes * 8) + ")")
                  .find(P.builtin()),
              std::string::npos);
  }
  const auto P = *x86CompactPlan(true, 16, 2, false, 0);
  for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
    auto F = med(P);
    auto &Op = F.Blocks[0].Ops[0];
    auto E = high(P);
    switch (Mutation) {
    case 0:
      Op.Inputs[4].ConstVal = 0x104;
      E->Operands[3]->ConstVal = 0x104;
      break;
    case 1:
      Op.Inputs[4].ConstVal = 0x44;
      E->Operands[3]->ConstVal = 0x44;
      break;
    case 2:
      Op.Inputs[4].ConstVal = 0x11;
      E->Operands[3]->ConstVal = 0x11;
      break;
    case 3:
      Op.Inputs[4].Size = 2;
      E->Operands[3]->Type = NdType::makeInt(2, false);
      break;
    case 4:
      Op.Inputs[1].Size = 4;
      E->Operands[0]->Type = NdType::makeInt(4, false);
      break;
    case 5:
      Op.NumInputs--;
      E->Operands.pop_back();
      break;
    case 6:
      Op.Output.Size = 32;
      E->Type = NdType::makeInt(32, false);
      break;
    case 7:
      Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
      E->MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case 8:
      Op.Inputs[4] = temporary(5, 1);
      E->Operands[3] = HighExpr::makeVar(temporary(5, 1));
      break;
    }
    EXPECT_FALSE(
        x86CompactShapeIsValid(id(P), x86CompactMedShape(Op, Arch::X64)));
    EXPECT_THROW(x86CompactHighPlan(Arch::X64, *E), std::invalid_argument);
    llvm::LLVMContext C;
    EXPECT_THROW(MedLLVMEmitter().emit({F}, C, "invalid", Arch::X64),
                 std::invalid_argument);
  }
  for (unsigned Stored = 0; Stored < 5; ++Stored) {
    auto F = med(P);
    auto &Op = F.Blocks[0].Ops[0];
    Op.Inputs.resize(Stored);
    EXPECT_FALSE(
        x86CompactShapeIsValid(id(P), x86CompactMedShape(Op, Arch::X64)));
  }
}
TEST(X86CompressExpandSource, OwnedCallsRejectMutatedEffectsAndControls) {
  const auto P = *x86CompactPlan(true, 16, 2, false, 0);
  for (bool Native : {false, true}) {
    if (Native && !llvm::Intrinsic::lookupIntrinsicID(P.llvmName()))
      continue;
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      llvm::LLVMContext C;
      llvm::Module M("invalid-compact", C);
      llvm::IRBuilder<> B(C);
      auto *F =
          llvm::Function::Create(llvm::FunctionType::get(B.getVoidTy(), false),
                                 llvm::GlobalValue::ExternalLinkage, "bad", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
      auto *V = llvm::FixedVectorType::get(B.getInt32Ty(), 4);
      auto *I = call(B, M, P, llvm::ConstantPointerNull::get(B.getPtrTy()),
                     B.getInt64(0), llvm::Constant::getNullValue(V), Native);
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
      case 5:
        if (Native)
          I->setArgOperand(3, B.getInt32(6));
        else {
          const auto *Asm = llvm::cast<llvm::InlineAsm>(I->getCalledOperand());
          I->setCalledOperand(
              llvm::InlineAsm::get(Asm->getFunctionType(), Asm->getAsmString(),
                                   Asm->getConstraintString(), false));
        }
        break;
      }
      EXPECT_THROW(x86CompactCallPlan(A, *I), std::invalid_argument);
      CEmitterOptions O;
      O.TheArch = A;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      EXPECT_THROW(LLVMCEmitter().emit(M, OS, O), std::invalid_argument);
      EXPECT_TRUE(Source.empty());
    }
  }
}

TEST(X86CompressExpandSource, UnrecoveredInputsCannotBecomeZero) {
  const auto P = *x86CompactPlan(true, 16, 2, false, 0);
  for (unsigned Input : {1u, 2u, 3u})
    for (bool Indirect : {false, true}) {
      auto F = med(P);
      const auto Unknown =
          temporary(999, F.Blocks[0].Ops[0].Inputs[Input].Size);
      F.Blocks[0].Ops[0].Inputs[Input] = Unknown;
      if (Indirect) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output = temporary(1000, Unknown.Size);
        Copy.Output.SSAVer = 1;
        Copy.addInput(Unknown);
        F.Blocks[0].Ops[0].Inputs[Input] = Copy.Output;
        F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin(), Copy);
      }
      llvm::LLVMContext C;
      EXPECT_THROW(MedLLVMEmitter().emit({F}, C, "unknown", Arch::X64),
                   std::invalid_argument);
      MedLLVMEmitter SourceEmitter;
      SourceEmitter.setPreserveUnknownSourceValues(true);
      auto M = SourceEmitter.emit({F}, C, "analysis", Arch::X64);
      ASSERT_TRUE(M);
      ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
      bool Retained = false;
      for (const auto &Function : *M)
        for (const auto &Block : Function)
          for (const auto &Instruction : Block)
            Retained |= llvm_source_unknown::isValue(&Instruction);
      EXPECT_TRUE(Retained);
    }
  auto F = med(P);
  auto Parameter = temporary(0, 8);
  Parameter.Kind = MedVar::Param;
  Parameter.RegOff = kNoParamReg;
  F.Params.push_back(Parameter);
  F.Blocks[0].Ops[0].Inputs[2] = Parameter;
  llvm::LLVMContext C;
  auto M = MedLLVMEmitter().emit({F}, C, "known-parameter", Arch::X64);
  ASSERT_TRUE(M);
  ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
}

TEST(X86CompressExpandSource, SuppressedOperandsNeedNoIncomingState) {
  for (unsigned Mode : {0u, 1u, 2u, 3u}) {
    const auto P = *x86CompactPlan(Mode != 0, 16, 2, Mode >= 2, 0x64);
    const uint64_t Mask = Mode == 1 ? 15 : Mode == 3 ? 5 : 0;
    auto F = med(P);
    auto &Op = F.Blocks[0].Ops[0];
    Op.Inputs[2] = MedVar::makeConst(Mask, 8);
    if (!Mask)
      Op.Inputs[1] = temporary(998, 8);
    Op.Inputs[3] = temporary(999, 16);
    llvm::LLVMContext C;
    auto M = MedLLVMEmitter().emit({F}, C, "suppressed", Arch::X64);
    ASSERT_TRUE(M);
    ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
    for (const auto &Function : *M)
      for (const auto &Block : Function)
        for (const auto &Instruction : Block)
          EXPECT_FALSE(llvm_source_unknown::isValue(&Instruction));
    // A dead private COPY must not make an unobserved entry value required.
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = temporary(1000, 16);
    Copy.Output.SSAVer = 1;
    Copy.addInput(temporary(999, 16));
    Op.Inputs[3] = Copy.Output;
    F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin(), Copy);
    EXPECT_NO_THROW(
        MedLLVMEmitter().emit({F}, C, "suppressed-copy", Arch::X64));

    auto E = high(P);
    E->Operands[1] = HighExpr::makeConst(Mask, 8);
    const auto Text = renderX86CompactExpression(
        Arch::X64, *E,
        {"unknown_address", std::to_string(Mask), "unknown_prior", "control"},
        "unsigned _BitInt(128)");
    EXPECT_NE(Text.find(P.builtin()), std::string::npos);
    EXPECT_EQ(Text.find("unknown_prior"), std::string::npos);
    EXPECT_EQ(Text.find("unknown_address") == std::string::npos, !Mask);
    // Calls and loads which produce an unused register value still own their
    // independent effects. They must not disappear with the unread operand.
    E->Operands[2] = HighExpr::makeCall("produce_data", 0, {});
    E->Operands[2]->Type = NdType::makeInt(16, false);
    const auto Effect = renderX86CompactExpression(
        Arch::X64, *E,
        {"address", std::to_string(Mask), "produce_data()", "control"},
        "unsigned _BitInt(128)");
    EXPECT_NE(Effect.find("produce_data()"), std::string::npos);
  }
}

TEST(X86CompressExpandSource, ExportsNativeAndCompatibleWrappers) {
  const auto *Dir = std::getenv("NEVERD_COMPACT_EXPORT_DIR");
  if (Dir)
    ASSERT_FALSE(llvm::sys::fs::create_directories(Dir));
  for (Arch A : {Arch::X64, Arch::X86}) {
    llvm::LLVMContext C;
    llvm::Module M("compact-wrappers", C);
    M.setTargetTriple(llvm::Triple(A == Arch::X64 ? "x86_64-unknown-linux-gnu"
                                                  : "i386-unknown-linux-gnu"));
    std::string High =
        "#include <stdint.h>\n" + std::string(X86CompactCompilerDispatch);
    unsigned Index = 0;
    for (const auto &P : plans(A)) {
      const auto Name = "compact_" + std::to_string(Index++);
      llvm::IRBuilder<> B(C);
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(
              B.getVoidTy(),
              {B.getPtrTy(), B.getInt64Ty(), B.getPtrTy(), B.getPtrTy()},
              false),
          llvm::GlobalValue::ExternalLinkage, Name, M);
      std::string Features, CFeatures;
      for (const auto &Feature : P.features())
        Features += (Features.empty() ? "+" : ",+") + Feature;
      for (const auto &Feature : x86CompactSourceFeatures(P))
        CFeatures += (CFeatures.empty() ? "" : ",") + Feature;
      F->addFnAttr("target-features", Features);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
      auto *V = llvm::FixedVectorType::get(B.getInt32Ty(), P.Bytes / 4);
      auto *Data = B.CreateAlignedLoad(V, F->getArg(2), llvm::Align(1));
      const bool Native =
          llvm::Intrinsic::lookupIntrinsicID(P.llvmName()) &&
          (!P.stackReference() || x86NativeStackMemoryIsAvailable());
      auto *R = call(B, M, P, F->getArg(0), F->getArg(1), Data, Native);
      if (P.Expand)
        B.CreateAlignedStore(R, F->getArg(3), llvm::Align(1));
      call(B, M, P, F->getArg(0), B.getInt64(0), Data,
           Native); // dead and zero mask remain native
      B.CreateRetVoid();
      auto E = high(P);
      const auto Type = "unsigned _BitInt(" + std::to_string(P.Bytes * 8) + ")";
      High += "__attribute__((target(\"" + CFeatures + "\"))) void " + Name +
              "(void *address, uint64_t mask, void *input, void *output) { " +
              Type + " data; __builtin_memcpy(&data,input," +
              std::to_string(P.Bytes) + "); ";
      auto Expr = renderX86CompactExpression(
          A, *E, {"address", "mask", "data", "control"}, Type);
      if (P.Expand)
        High += Type + " result = " + Expr +
                "; __builtin_memcpy(output,&result," + std::to_string(P.Bytes) +
                "); ";
      else
        High += Expr + "; ";
      High += "(void)" +
              renderX86CompactExpression(
                  A, *E, {"address", "0", "data", "control"}, Type) +
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
TEST(X86CompressExpandSource, PipelineRetainsOriginalOpcodeFamily) {
  const auto *Dir = std::getenv("NEVERD_COMPACT_EXPORT_DIR");
  if (Dir)
    ASSERT_FALSE(
        llvm::sys::fs::create_directories(std::string(Dir) + "/pipeline"));
  unsigned Index = 0;
  for (const auto &P : plans()) {
    const uint8_t Opcode = P.Kind < 2   ? (P.Expand ? 0x62 : 0x63)
                           : P.Kind < 4 ? (P.Expand ? 0x89 : 0x8b)
                                        : (P.Expand ? 0x88 : 0x8a);
    // Supply the address, mask and vector through ordinary SysV pointer/
    // integer arguments. A naked instruction using entry RAX/K2 would have
    // unknown inputs, so successful assembly alone would not qualify its C.
    std::vector<uint8_t> Bytes = {
        0x62,
        0xf2,
        uint8_t(P.Kind & 1 ? 0xfd : 0x7d),
        uint8_t(0x0a |
                (P.Bytes == 16   ? 0
                 : P.Bytes == 32 ? 0x20
                                 : 0x40) |
                (P.Zero ? 0x80 : 0)),
        Opcode,
        uint8_t((P.Expand ? 0x08 : 0x18) | (P.stackReference() ? 0x45 : 0)),
        0xc3};
    if (P.stackReference())
      Bytes.insert(Bytes.end() - 1, 0); // RBP + disp8(0).
    if (P.hasSegmentPrefix())
      Bytes.insert(Bytes.begin(), x86NativeMemoryPrefix(P.Prefix));
    std::vector<uint8_t> Setup = {0x48, 0x89, 0xf8,        // mov rax,rdi
                                  0xc5, 0xf8, 0x92, 0xd6}; // kmovw k2,esi
    if (P.stackReference()) {
      // Preserve RBP and retain the known RAX return of the ordinary fixture.
      Setup.insert(Setup.begin(), {0x55, 0x48, 0x89, 0xfd});
    }
    const std::vector<uint8_t> Load =
        P.Bytes == 64 ? std::vector<uint8_t>{0x62, 0xf1, 0xfe, 0x48, 0x6f}
                      : std::vector<uint8_t>{
                            0xc5, uint8_t(P.Bytes == 16 ? 0xfa : 0xfe), 0x6f};
    Setup.insert(Setup.end(), Load.begin(), Load.end());
    Setup.push_back(P.Expand ? 0x0a : 0x1a); // vmovdqu[64] xmm/ymm/zmm1/3,[rdx]
    Bytes.insert(Bytes.begin(), Setup.begin(), Setup.end());
    if (P.Expand) {
      auto Store = Load;
      Store.back() = 0x7f;
      Store.push_back(0x09); // vmovdqu[64] [rcx],xmm/ymm/zmm1
      Bytes.insert(Bytes.end() - 1, Store.begin(), Store.end());
    }
    if (P.stackReference())
      Bytes.insert(Bytes.end() - 1, 0x5d); // pop rbp.
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
    Symbol.Name = "compact_pipeline";
    Image.Symbols.push_back(Symbol);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {0x1000};
    llvm::LLVMContext C;
    auto R = Pipeline().run(Image, C, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    auto M = MedLLVMEmitter().emit(R.MedFuncs, C, "compact", Arch::X64, {},
                                   &Image, BinaryFormat::ELF);
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
  EXPECT_EQ(Index, 216u);
}
} // namespace
