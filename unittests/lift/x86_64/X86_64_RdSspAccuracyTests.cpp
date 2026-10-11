//===- X86_64_RdSspAccuracyTests.cpp - Shadow stack read regressions ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/backend/llvm/LLVMX86ShadowStackAsm.h"
#include "neverd/ir/high/X86ShadowStackShape.h"
#include "neverd/lift/X86Lifter.h"

namespace {

std::vector<uint8_t> rdsspBytes(unsigned Register, unsigned Width,
                                Arch Target) {
  std::vector<uint8_t> Bytes{0xf3};
  const unsigned Rex = (Width == 8 ? 8 : 0) | (Register >= 8 ? 1 : 0);
  if (Target == Arch::X64 && Rex)
    Bytes.push_back(0x40 | Rex);
  Bytes.insert(Bytes.end(), {0x0f, 0x1e, uint8_t(0xc8 | (Register & 7))});
  return Bytes;
}

TEST(X86RdSspContract, EncodedDestinationOwnsItsFullConditionalResult) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Width : {4U, 8U}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (unsigned Register = 0; Register < (Target == Arch::X64 ? 16 : 8);
           ++Register) {
        SCOPED_TRACE(static_cast<unsigned>(Target));
        SCOPED_TRACE(Width);
        SCOPED_TRACE(Register);
        Decoder Decode;
        ASSERT_TRUE(Decode.init(Target));
        Decode.setStrict(true);
        const auto Bytes = rdsspBytes(Register, Width, Target);
        DecodedInsn Instruction;
        ASSERT_EQ(
            Decode.decodeOne(Bytes.data(), Bytes.size(), 0x1000, Instruction),
            static_cast<int>(Bytes.size()));
        ASSERT_EQ(Instruction.Id, Width == 8 ? X86_INS_RDSSPQ : X86_INS_RDSSPD);
        std::vector<LowOp> Ops;
        Decode.liftToLow(Instruction, Ops);
        const auto Found =
            std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
              return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                     Op.Inputs[0].isConst() &&
                     Op.Inputs[0].Offset == unsigned(Intrinsic::CetRdSsp);
            });
        ASSERT_NE(Found, Ops.end());
        EXPECT_EQ(Found->NumInputs, 3);
        const auto Encoded = mapCapstoneReg(
            static_cast<x86_reg>(Instruction.Raw->detail->x86.operands[0].reg));
        const unsigned FullBytes = Target == Arch::X64 ? 8 : 4;
        EXPECT_TRUE(Found->Output.isReg());
        EXPECT_EQ(Found->Output.Offset, Encoded.Offset);
        EXPECT_EQ(Found->Output.Size, FullBytes);
        if (Found->NumInputs != 3)
          continue;
        EXPECT_EQ(Found->Inputs[1], NdVar::reg(Encoded.Offset, FullBytes));
        EXPECT_EQ(Found->Inputs[2], NdVar::cst(Width, 1));
        // Disabled shadow stacks turn even RDSSPD into a full-register NOP;
        // unconditional r32 zero-extension would destroy that old high half.
        EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
          return Op.Opcode == NdOp::INT_ZEXT && Op.Output.isReg() &&
                 Op.Output.Offset == Encoded.Offset;
        }));
        if (Target == Arch::X64) {
          X86Lifter Checked(Target);
          Checked.setStrict(true);
          LowInstructionUndefinedEffects Undefined;
          LowInstructionPreservedState Preserved;
          std::vector<LowOp> Audited;
          Checked.lift(Instruction.Raw, Audited, {}, {}, &Undefined,
                       &Preserved);
          EXPECT_EQ(Undefined.Coverage, LowUndefinedCoverage::Missing);
          EXPECT_EQ(Preserved.Audit, LowPreservedStateAudit::Missing);
        }
      }
    }
}

LowOp rdsspOp(unsigned Register, unsigned Width, Arch Target = Arch::X64) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output =
      NdVar::reg(x86reg::generalReg(Register), Target == Arch::X64 ? 8 : 4);
  Op.addInput(NdVar::cst(unsigned(Intrinsic::CetRdSsp), 2));
  Op.addInput(Op.Output);
  Op.addInput(NdVar::cst(Width, 1));
  return Op;
}

TEST(X86RdSspContract, InconsistentDecodedDescriptorsRefuseInBothStrictModes) {
  for (bool Strict : {false, true})
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Arch::X64));
      const auto Bytes = rdsspBytes(9, 4, Arch::X64);
      DecodedInsn Insn;
      ASSERT_EQ(Decode.decodeOne(Bytes.data(), Bytes.size(), Entry, Insn),
                Bytes.size());
      auto *Raw = const_cast<cs_insn *>(Insn.Raw);
      auto &X86 = Raw->detail->x86;
      switch (Mutation) {
      case 0:
        X86.operands[0].reg = X86_REG_R9D;
        X86.operands[0].size = 8;
        break;
      case 1:
        X86.operands[0].reg = X86_REG_ECX;
        break;
      case 2:
        X86.operands[0].type = X86_OP_MEM;
        break;
      case 3:
        X86.op_count = 0;
        break;
      case 4:
        Raw->id = X86_INS_RDSSPQ;
        break;
      case 5:
        Raw->bytes[1] |= 8;
        break;
      case 6:
        Raw->bytes[Raw->size - 1] ^= 1;
        break;
      case 7:
        Raw->size = 16;
        break;
      }
      X86Lifter Lift(Arch::X64);
      Lift.setStrict(Strict);
      std::vector<LowOp> Ops;
      EXPECT_THROW(Lift.lift(Raw, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
}

TEST(X86RdSspContract, MedVerifierAndHighShapeRejectInventedResults) {
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = {.Kind = MedVar::Temp, .TheArch = Arch::X64, .Id = 1, .Size = 8};
  Op.addInput(MedVar::makeConst(unsigned(Intrinsic::CetRdSsp), 2));
  Op.addInput(MedVar::makeConst(UINT64_C(0x1234567876543210), 8));
  Op.addInput(MedVar::makeConst(4, 1));
  MedBlock Block;
  Block.Id = 0;
  Block.Ops.push_back(Op);
  MedFunc Function;
  Function.Blocks.push_back(Block);
  EXPECT_TRUE(verifyMedFunc(Function, "valid RDSSP"));
  Function.Blocks[0].Ops[0].IntrinsicOutputs.push_back(
      MedVar{.Kind = MedVar::Temp, .Id = 2, .Size = 8});
  EXPECT_FALSE(verifyMedFunc(Function, "invented auxiliary SSP"));
  Function.Blocks[0].Ops[0] = Op;
  Function.Blocks[0].Ops[0].Inputs[1].TheArch = Arch::AArch64;
  EXPECT_FALSE(verifyMedFunc(Function, "foreign old destination"));
  Function.Blocks[0].Ops[0] = Op;
  Function.Blocks[0].Ops[0].Output.Size = 4;
  EXPECT_FALSE(verifyMedFunc(Function, "lost old high half"));

  HighExpr Call;
  Call.Kind = ExprKind::Call;
  Call.IntrinsicId = Intrinsic::CetRdSsp;
  Call.Type = NdType::makeInt(8, false);
  Call.Operands = {HighExpr::makeConst(7, 8), HighExpr::makeConst(4, 1)};
  EXPECT_TRUE(x86ShadowStackReadShapeIsValid(
      x86ShadowStackReadHighShape(Call, Arch::X64)));
  Call.Operands[0]->Type = NdType::makeFloat(8);
  EXPECT_FALSE(x86ShadowStackReadShapeIsValid(
      x86ShadowStackReadHighShape(Call, Arch::X64)));
  Call.Operands[0]->Type = NdType::makePtr();
  EXPECT_TRUE(x86ShadowStackReadShapeIsValid(
      x86ShadowStackReadHighShape(Call, Arch::X64)));
  Call.IsIndirectCall = true;
  EXPECT_FALSE(x86ShadowStackReadShapeIsValid(
      x86ShadowStackReadHighShape(Call, Arch::X64)));
}

TEST(X86RdSspContract, OwnedAssemblyRequiresTheFullTiedCarrier) {
  llvm::LLVMContext Context;
  llvm::Module Module("rdssp-contract", Context);
  for (unsigned Word : {4U, 8U})
    for (unsigned Width : {4U, 8U}) {
      if (Word == 4 && Width == 8)
        continue;
      auto *Ty = llvm::Type::getIntNTy(Context, Word * 8);
      auto *Signature = llvm::FunctionType::get(Ty, {Ty}, false);
      auto *Fn = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, "read", Module);
      llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Fn));
      const auto Assembly = x86ShadowStackReadAsm(Word, Width);
      auto *Asm = llvm::InlineAsm::get(Signature, Assembly,
                                       X86ShadowStackReadConstraints, true);
      auto *Call = B.CreateCall(Asm, {Fn->getArg(0)});
      B.CreateRet(Call);
      EXPECT_FALSE(classifyX86ShadowStackReadAsm(*Call));
      Call->setMetadata(X86ShadowStackReadMetadata,
                        llvm::MDNode::get(Context, {}));
      EXPECT_EQ(classifyX86ShadowStackReadAsm(*Call), Width);
      Call->setCalledOperand(
          llvm::InlineAsm::get(Signature, Assembly, "=r,r", true));
      EXPECT_DEATH(classifyX86ShadowStackReadAsm(*Call),
                   "invalid x86 shadow stack read assembly contract");
      Call->setCalledOperand(llvm::InlineAsm::get(
          Signature, Assembly, X86ShadowStackReadConstraints, false));
      EXPECT_DEATH(classifyX86ShadowStackReadAsm(*Call),
                   "invalid x86 shadow stack read assembly contract");
      Call->setCalledOperand(Asm);
      Call->setMetadata(
          X86ShadowStackReadMetadata,
          llvm::MDNode::get(Context, llvm::MDString::get(Context, "forged")));
      EXPECT_DEATH(classifyX86ShadowStackReadAsm(*Call),
                   "invalid x86 shadow stack read assembly contract");
    }
}

TEST(X86RdSspContract, ExplicitEnablementDefinesTheConditionalFullRegister) {
  for (Arch Target : {Arch::X86, Arch::X64}) {
    BinaryImage Image;
    Image.Arch = Target;
    NdOpEmulator Emu(Image);
    for (unsigned Width : {4U, 8U}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (unsigned Register = 0; Register < (Target == Arch::X64 ? 16 : 8);
           ++Register)
        for (unsigned CPL = 0; CPL < 4; ++CPL)
          for (bool Enabled : {false, true}) {
            SCOPED_TRACE(static_cast<unsigned>(Target));
            SCOPED_TRACE(Width);
            SCOPED_TRACE(Register);
            SCOPED_TRACE(CPL);
            SCOPED_TRACE(Enabled);
            const auto Op = rdsspOp(Register, Width, Target);
            const uint64_t Old =
                Target == Arch::X64 ? UINT64_C(0xfedcba9876543210) : 0x76543210;
            const uint64_t SSP =
                Target == Arch::X64 ? UINT64_C(0x1234567887654320) : 0x87654320;
            Emu.reset();
            Emu.setRegister(Op.Output.Offset, Old);
            for (const auto Flag : x86reg::EFlagsBits)
              Emu.setRegister(Flag.Flag, Flag.Bit & 1);
            ASSERT_TRUE(Emu.setX86ShadowStackContext(CPL, Enabled, SSP));
            ASSERT_TRUE(Emu.step(Op));
            EXPECT_EQ(Emu.getRegister(Op.Output.Offset),
                      Enabled ? (Width == 4 ? uint32_t(SSP) : SSP) : Old);
            for (const auto Flag : x86reg::EFlagsBits)
              EXPECT_EQ(Emu.getRegister(Flag.Flag), Flag.Bit & 1);
            EXPECT_FALSE(Emu.skips().any());
          }
    }
  }
}

TEST(X86RdSspContract, UnknownStateRefusesWithoutDefiningOrClobberingAResult) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  NdOpEmulator Emu(Image);
  const auto Op = rdsspOp(9, 4);
  Emu.setRegister(Op.Output.Offset, UINT64_C(0xcafef00d87654321));
  EXPECT_FALSE(Emu.step(Op));
  EXPECT_EQ(Emu.getRegister(Op.Output.Offset), UINT64_C(0xcafef00d87654321));
  ASSERT_TRUE(Emu.setX86ShadowStackContext(3, true));
  EXPECT_FALSE(Emu.step(Op));
  EXPECT_EQ(Emu.getRegister(Op.Output.Offset), UINT64_C(0xcafef00d87654321));
  // Enabled RDSSP does not depend on the incoming GPR's numerical value.
  Emu.reset();
  ASSERT_TRUE(
      Emu.setX86ShadowStackContext(3, true, UINT64_C(0x1234567800000040)));
  ASSERT_TRUE(Emu.step(Op));
  EXPECT_EQ(Emu.getRegister(Op.Output.Offset), 0x40U);
  Emu.reset();
  ASSERT_TRUE(Emu.setX86ShadowStackContext(3, false));
  EXPECT_FALSE(Emu.step(Op));
  EXPECT_FALSE(Emu.getRegister(Op.Output.Offset));
}

TEST(X86RdSspContract, CallsAndCetMutatorsInvalidateAuthenticatedEnablement) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  NdOpEmulator Emu(Image);
  const auto Read = rdsspOp(3, 8);
  Emu.setCallPreservedRegisters({Read.Output.Offset});
  for (bool Enabled : {false, true})
    for (auto Code : {NdOp::CALL, NdOp::INDIR_CALL}) {
      Emu.reset();
      Emu.setRegister(Read.Output.Offset, 0x98765432);
      ASSERT_TRUE(Emu.setX86ShadowStackContext(3, Enabled, 0x1000));
      LowOp Call;
      Call.Opcode = Code;
      ASSERT_TRUE(Emu.step(Call));
      EXPECT_FALSE(Emu.step(Read));
      EXPECT_EQ(Emu.getRegister(Read.Output.Offset), 0x98765432U);
      ASSERT_TRUE(Emu.setX86ShadowStackContext(3, Enabled, 0x2000));
      ASSERT_TRUE(Emu.step(Read));
      EXPECT_EQ(Emu.getRegister(Read.Output.Offset),
                Enabled ? 0x2000U : 0x98765432U);
    }
  for (auto Id :
       {Intrinsic::CetIncSsp, Intrinsic::CetRstorssp, Intrinsic::CetSaveprevssp,
        Intrinsic::CetSetssbsy, Intrinsic::CetClrssbsy}) {
    ASSERT_TRUE(Emu.setX86ShadowStackContext(3, true, 0x1000));
    LowOp Effect;
    Effect.Opcode = NdOp::INTRINSIC;
    Effect.addInput(NdVar::cst(unsigned(Id), 2));
    EXPECT_FALSE(Emu.step(Effect));
    EXPECT_FALSE(Emu.step(Read));
  }
  ASSERT_TRUE(Emu.setX86ShadowStackContext(3, false));
  ASSERT_TRUE(Emu.setX86EnqueueContext(0, 0x80000000, 48));
  EXPECT_FALSE(Emu.step(Read));
  EXPECT_FALSE(Emu.setX86ShadowStackContext(4, false));
  EXPECT_FALSE(Emu.step(Read));
}

TEST(X86RdSspContract, MalformedShapesRefuseBeforeStateOrDestinationChanges) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  NdOpEmulator Emu(Image);
  ASSERT_TRUE(Emu.setX86ShadowStackContext(3, true, 0x1000));
  const auto Valid = rdsspOp(8, 4);
  Emu.setRegister(Valid.Output.Offset, UINT64_C(0x1234567887654321));
  std::vector<LowOp> Invalid;
  for (unsigned Width : {0U, 1U, 2U, 16U}) {
    auto Op = Valid;
    Op.Inputs[2].Offset = Width;
    Invalid.push_back(Op);
  }
  auto Add = [&](auto Change) {
    auto Op = Valid;
    Change(Op);
    Invalid.push_back(Op);
  };
  Add([](auto &O) { O.NumInputs = 1; });
  Add([](auto &O) { O.NumInputs = 4; });
  Add([](auto &O) { O.Inputs[0].Size = 4; });
  Add([](auto &O) { O.Inputs[0].Offset |= UINT64_C(1) << 16; });
  Add([](auto &O) { O.Inputs[1].Size = 4; });
  Add([](auto &O) { O.Output.Size = 4; });
  Add([](auto &O) { O.Output = NdVar::cst(7, 8); });
  Add([](auto &O) { O.Inputs[1] = NdVar::ram(0x1000, 8); });
  Add([](auto &O) { O.Inputs[2].Size = 4; });
  Add([](auto &O) { O.Inputs[2] = NdVar::reg(x86reg::RAX, 1); });
  Add([](auto &O) { O.MemoryAddressSpace = NdMemoryAddressSpace::X86FS; });
  Add([](auto &O) {
    O.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  });
  for (const auto &Op : Invalid) {
    EXPECT_FALSE(Emu.step(Op));
    EXPECT_EQ(Emu.getRegister(Valid.Output.Offset),
              UINT64_C(0x1234567887654321));
  }
  BinaryImage X86Image;
  X86Image.Arch = Arch::X86;
  NdOpEmulator X86Emu(X86Image);
  EXPECT_FALSE(X86Emu.setX86ShadowStackContext(3, true, UINT64_C(1) << 32));
  ASSERT_TRUE(X86Emu.setX86ShadowStackContext(3, true, 0x1000));
  EXPECT_FALSE(X86Emu.step(rdsspOp(0, 8, Arch::X86)));
}

// Generated code reads host SSP. These executions authenticate a disabled
// host profile; they do not certify guest CALL/RET topology with CET enabled.
class X86RdSspAccuracy : public X86FPStateFixture {
protected:
  bool StackPointerProbe = false;
  void SetUp() override {
    X86FPStateFixture::SetUp();
    if (testing::Test::IsSkipped() || testing::Test::HasFatalFailure())
      return;
    if (!nativeX64())
      GTEST_SKIP() << "native x86-64 host required";
    const auto Source = file("authenticate-cet-disabled.c");
    const auto Executable = file("authenticate-cet-disabled.exe");
    write(Source, R"(
#include <stdint.h>
int main(void) {
  uint64_t a = UINT64_C(0xfedcba9876543210), b = UINT64_C(0x1234567887654320);
  __asm__ volatile("rdsspd %k0" : "+r"(a) :: "memory");
  __asm__ volatile("rdsspq %0" : "+r"(b) :: "memory");
  return a == UINT64_C(0xfedcba9876543210) && b == UINT64_C(0x1234567887654320) ? 0 : 77;
}
)");
    const auto Built = command(Compiler, {"-O0", Source, "-o", Executable});
    ASSERT_EQ(Built.Status, 0) << Built.Error;
    const auto Run = command(Executable, {});
    if (Run.Status == 77)
      GTEST_SKIP() << "host shadow stacks are enabled; disabled-profile oracle "
                      "unavailable";
    ASSERT_EQ(Run.Status, 0) << Run.Error;
  }

  std::string comparisonDriver(bool, bool Declare, bool,
                               bool = false) override {
    std::string Text = R"(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern void native_probe(uintptr_t);
)";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += R"(
int main(void) {
  uint64_t probe32 = UINT64_C(0xfedcba9876543210), probe64 = UINT64_C(0x1234567887654320);
  __asm__ volatile("rdsspd %k0" : "+r"(probe32) :: "memory");
  __asm__ volatile("rdsspq %0" : "+r"(probe64) :: "memory");
  if (probe32 != UINT64_C(0xfedcba9876543210) || probe64 != UINT64_C(0x1234567887654320)) return 77;
  const uint64_t seeds[] = {0, UINT64_C(0xfedcba9876543210), UINT64_C(0x1234567887654321), UINT64_MAX};
  for (unsigned i = 0; i < sizeof(seeds)/sizeof(seeds[0]); ++i) {
    uint64_t expected[4] = {seeds[i], 0, 0, 0}, actual[4];
    memcpy(actual, expected, sizeof(actual));
    native_probe((uintptr_t)expected);
    isa_probe((uintptr_t)actual);
    /* Only the arithmetic flags defined by CMP are observations. */
)";
    if (StackPointerProbe)
      Text += "if (expected[0] != expected[2] || actual[0] != actual[2]) "
              "return 2;\n"
              "expected[0] = expected[2] = actual[0] = actual[2] = seeds[i];\n";
    return Text + R"(
    expected[1] &= 0x8d5; expected[3] &= 0x8d5;
    actual[1] &= 0x8d5; actual[3] &= 0x8d5;
    if (memcmp(actual, expected, sizeof(actual)) || expected[2] != seeds[i] || expected[1] != expected[3]) {
      printf("seed=%llx expected=%llx actual=%llx flags=%llx/%llx\n", (unsigned long long)seeds[i],
          (unsigned long long)expected[2], (unsigned long long)actual[2],
          (unsigned long long)expected[3], (unsigned long long)actual[3]);
      return 1;
    }
  }
  return 0;
}
)";
  }

  static std::vector<uint8_t> registerProbe(unsigned Register, unsigned Width) {
    const unsigned Argument = hostFormat() == BinaryFormat::COFF ? 1 : 7;
    const unsigned Base = Register == 11 ? 10 : 11;
    std::vector<uint8_t> Bytes;
    // mov base,arg. Save only a callee-preserved destination, then load seed.
    Bytes.insert(Bytes.end(),
                 {uint8_t(0x49 | (Argument >= 8 ? 4 : 0)), 0x89,
                  uint8_t(0xc0 | ((Argument & 7) << 3) | (Base & 7))});
    const bool Preserved = Register == 3 || Register == 5 || Register >= 12 ||
                           (hostFormat() == BinaryFormat::COFF &&
                            (Register == 6 || Register == 7));
    if (Preserved) {
      if (Register >= 8)
        Bytes.push_back(0x41);
      Bytes.push_back(0x50 | (Register & 7));
    }
    Bytes.insert(Bytes.end(), {uint8_t(0x49 | (Register >= 8 ? 4 : 0)), 0x8b,
                               uint8_t(((Register & 7) << 3) | (Base & 7))});
    // cmp $1,destination; pushfq; popq 8(base).
    Bytes.insert(Bytes.end(), {uint8_t(0x48 | (Register >= 8 ? 1 : 0)), 0x83,
                               uint8_t(0xf8 | (Register & 7)), 1, 0x9c, 0x41,
                               0x8f, uint8_t(0x40 | (Base & 7)), 8});
    const auto Read = rdsspBytes(Register, Width, Arch::X64);
    Bytes.insert(Bytes.end(), Read.begin(), Read.end());
    Bytes.insert(Bytes.end(),
                 {uint8_t(0x49 | (Register >= 8 ? 4 : 0)), 0x89,
                  uint8_t(0x40 | ((Register & 7) << 3) | (Base & 7)), 16, 0x9c,
                  0x41, 0x8f, uint8_t(0x40 | (Base & 7)), 24});
    if (Preserved) {
      if (Register >= 8)
        Bytes.push_back(0x41);
      Bytes.push_back(0x58 | (Register & 7));
    }
    Bytes.push_back(0xc3);
    return Bytes;
  }
};

TEST_F(X86RdSspAccuracy, IntegerReturnsRetainTheFullConditionalValue) {
  for (unsigned Width : {4U, 8U}) {
    std::vector<uint8_t> Bytes = {
        0x48, 0x89, uint8_t(hostFormat() == BinaryFormat::COFF ? 0xc8 : 0xf8)};
    const auto Read = rdsspBytes(0, Width, Arch::X64);
    Bytes.insert(Bytes.end(), Read.begin(), Read.end());
    Bytes.push_back(0xc3);
    auto NativeAssembly = assembly(Bytes);
    for (size_t I = 0;
         (I = NativeAssembly.find("isa_probe", I)) != std::string::npos;)
      NativeAssembly.replace(I, 9, "native_probe");
    const auto Native = file("return.s");
    const auto NativeObject = file("return.o");
    write(Native, NativeAssembly);
    auto Built = command(Compiler, {"-c", Native, "-o", NativeObject});
    ASSERT_EQ(Built.Status, 0) << Built.Error;
    const std::string Driver = R"(
#include <stdint.h>
extern uint64_t native_probe(uint64_t);
int main(void) {
  uint64_t probe = UINT64_C(0xfedcba9876543210);
  __asm__ volatile("rdsspd %k0" : "+r"(probe) :: "memory");
  if (probe != UINT64_C(0xfedcba9876543210)) return 77;
  const uint64_t seeds[] = {0, UINT64_MAX, UINT64_C(0xfedcba9876543210), UINT64_C(1) << 63};
  for (unsigned i = 0; i < 4; ++i)
    if (native_probe(seeds[i]) != seeds[i] || (uint64_t)isa_probe(seeds[i]) != seeds[i]) return 1;
  return 0;
}
)";
    for (bool NoOpt : {false, true}) {
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(LLVM);
        auto Image = image(Bytes);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.LiftMode = LLVM;
        Options.SourceProjection = LLVM;
        Options.NoOpt = NoOpt;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Entry};
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        CEmitterOptions Emission;
        Emission.TheArch = Arch::X64;
        Emission.Format = hostFormat();
        Emission.PreserveLLVMFunctionTypes = true;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVM) {
          ASSERT_NE(Result.LlvmModule, nullptr);
          auto *Fn = Result.LlvmModule->getFunction("isa_probe");
          ASSERT_NE(Fn, nullptr);
          ASSERT_TRUE(Fn->getReturnType()->isIntegerTy(64));
          ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, Emission));
          auto Object =
              Codegen().compile(*Result.LlvmModule, Arch::X64, hostFormat());
          ASSERT_TRUE(Object.Success);
          const auto ObjectPath = file("return-generated.o");
          write(ObjectPath, llvm::StringRef(reinterpret_cast<const char *>(
                                                Object.ObjectData.data()),
                                            Object.ObjectData.size()));
          const auto Harness = file("return-driver.c");
          write(Harness,
                "extern unsigned long long isa_probe(unsigned long long);\n" +
                    Driver);
          const auto Executable = file("return-native.exe");
          Built = command(Compiler, {"-O2", NativeObject, ObjectPath, Harness,
                                     "-o", Executable});
          ASSERT_EQ(Built.Status, 0) << Built.Error;
          const auto Actual = command(Executable, {});
          EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error;
        } else {
          ASSERT_EQ(Result.HighFuncs.size(), 1U);
          ASSERT_NE(Result.HighFuncs[0].ReturnType, nullptr);
          ASSERT_EQ(Result.HighFuncs[0].ReturnType->Kind, NdTypeKind::Int);
          ASSERT_EQ(Result.HighFuncs[0].ReturnType->Size, 8);
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emission));
        }
        const auto C = file("return-source.c");
        write(C, Source + Driver);
        for (const char *Optimization : {"-O0", "-O2"}) {
          const auto Executable = file("return-source.exe");
          Built = command(Compiler,
                          {Optimization, NativeObject, C, "-o", Executable});
          ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
          const auto Actual = command(Executable, {});
          EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error << Source;
        }
      }
    }
  }
}

TEST_F(X86RdSspAccuracy, StackPointerDestinationRetainsItsCompleteOldValue) {
  StackPointerProbe = true;
  const unsigned Argument = hostFormat() == BinaryFormat::COFF ? 1 : 7;
  for (unsigned Width : {4U, 8U}) {
    std::vector<uint8_t> Bytes = {0x49, 0x89, uint8_t(0xc3 | (Argument << 3)),
                                  0x49, 0x89, 0x23, // mov (r11),rsp
                                  0x4d, 0x39, 0xdb, // cmp r11,r11: known input
                                  0x9c, 0x41, 0x8f,
                                  0x43, 8};
    const auto Read = rdsspBytes(4, Width, Arch::X64);
    Bytes.insert(Bytes.end(), Read.begin(), Read.end());
    Bytes.insert(Bytes.end(),
                 {0x49, 0x89, 0x63, 16, 0x9c, 0x41, 0x8f, 0x43, 24, 0xc3});
    compareFloating(Bytes, false);
  }
}

TEST_F(X86RdSspAccuracy, HighCPointerResultAndFreshLocalCompileAndExecute) {
  std::vector<HighFunc> Functions;
  for (bool PointerResult : {false, true}) {
    const auto Input = HighExpr::makeVar(
        MedVar{.Kind = MedVar::Param, .Id = 0, .Size = 8}, NdType::makePtr());
    auto Call = HighExpr::makeCall("", 0, {Input, HighExpr::makeConst(4, 1)});
    Call->IntrinsicId = Intrinsic::CetRdSsp;
    Call->Type = PointerResult ? NdType::makePtr() : NdType::makeInt(8, false);
    HighFunc Function;
    Function.Name = PointerResult ? "pointer_read" : "integer_read";
    Function.ReturnType = Call->Type;
    Function.Params = {{"nd_ssp_value", Input->Type}};
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body.push_back(Return);
    Functions.push_back(Function);
  }
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = hostFormat();
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  executeSource(Source, R"(
int main(void) {
  unsigned char object[2];
  if (pointer_read(object) != object || integer_read(object) != (uintptr_t)object) return 1;
  return 0;
}
)");
}

class X86RdSspNative
    : public X86RdSspAccuracy,
      public testing::WithParamInterface<std::pair<unsigned, unsigned>> {};

TEST_P(X86RdSspNative, FullDestinationAndFlagsMatchOriginalBytes) {
  const auto [Register, Width] = GetParam();
  compareFloating(registerProbe(Register, Width), false);
}

INSTANTIATE_TEST_SUITE_P(
    GeneralRegisters, X86RdSspNative,
    testing::Values(std::pair{0U, 4U}, std::pair{0U, 8U}, std::pair{1U, 4U},
                    std::pair{1U, 8U}, std::pair{2U, 4U}, std::pair{2U, 8U},
                    std::pair{3U, 4U}, std::pair{3U, 8U}, std::pair{5U, 4U},
                    std::pair{5U, 8U}, std::pair{6U, 4U}, std::pair{6U, 8U},
                    std::pair{7U, 4U}, std::pair{7U, 8U}, std::pair{8U, 4U},
                    std::pair{8U, 8U}, std::pair{9U, 4U}, std::pair{9U, 8U},
                    std::pair{10U, 4U}, std::pair{10U, 8U}, std::pair{11U, 4U},
                    std::pair{11U, 8U}, std::pair{12U, 4U}, std::pair{12U, 8U},
                    std::pair{13U, 4U}, std::pair{13U, 8U}, std::pair{14U, 4U},
                    std::pair{14U, 8U}, std::pair{15U, 4U},
                    std::pair{15U, 8U}));

} // namespace
