//===- X86_64_FPScalarConvertStateTests.cpp - Scalar conversion completion ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original x64 integer/precision conversion bytes versus full-state output.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/backend/llvm/LLVMX86FPStateAsm.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/X86FPStateShape.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include <array>
#include <tuple>

namespace {
struct ConvertForm {
  bool Integer;
  unsigned SourceBytes;
  unsigned DestinationBytes;
  bool Vex;
  bool Memory;
};

std::vector<ConvertForm> convertForms() {
  std::vector<ConvertForm> Forms;
  for (bool Integer : {false, true})
    for (unsigned Source : {4u, 8u})
      for (unsigned Destination : {4u, 8u})
        if (Integer || Source != Destination)
          for (bool Vex : {false, true})
            for (bool Memory : {false, true})
              Forms.push_back({Integer, Source, Destination, Vex, Memory});
  return Forms;
}

std::string formName(const ConvertForm &F) {
  return std::string(F.Integer ? "Integer" : "Precision") +
         std::to_string(F.SourceBytes * 8) + "To" +
         std::to_string(F.DestinationBytes * 8) + (F.Vex ? "Vex" : "Legacy") +
         (F.Memory ? "Memory" : "Register");
}

LowOp scalarConvertOp(unsigned Control, uint32_t State, bool Memory = false) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(2, x86FPScalarConvertDestinationBytes(Control) + 4);
  Op.addInput(
      NdVar::cst(unsigned(Memory ? Intrinsic::X86FPScalarConvertMemoryState
                                 : Intrinsic::X86FPScalarConvertState),
                 2));
  Op.addInput(
      NdVar::tmp(1, Memory ? 8 : x86FPScalarConvertSourceBytes(Control)));
  Op.addInput(NdVar::cst(Control, 1));
  Op.addInput(NdVar::cst(State, 4));
  return Op;
}

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
std::pair<uint64_t, uint32_t> nativeConvert(uint64_t Bits, uint32_t State,
                                            unsigned Control);
#endif

TEST(ScalarConvertContract,
     PreExceptionsDoNotCommitPostExceptionFlagsOrResult) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (auto [Control, Bits, State, Raised] :
       {std::tuple{2U, UINT64_C(1), 0x1e80U, 2U},
        std::tuple{4U, UINT64_C(0x7f800071), 0x1f00U, 1U},
        std::tuple{3U, UINT64_C(0x1000001), 0x0f80U, 0x20U},
        std::tuple{2U, UINT64_C(0x36a0000000000000), 0x1780U, 0x10U}}) {
    SCOPED_TRACE(Control);
    for (unsigned Sticky : {0U, 0x25U}) {
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegister(1, Bits);
      const auto Op = scalarConvertOp(Control, State | Sticky);
      const std::vector<uint8_t> Before(Op.Output.Size, 0xa7);
      E.setRegisterBytes(2, Before);
      EXPECT_FALSE(E.step(Op));
      EXPECT_EQ(E.getMXCSR(), State | Sticky | Raised);
      ASSERT_TRUE(E.getRegisterBytes(2));
      EXPECT_EQ(*E.getRegisterBytes(2), Before);
    }
  }
}

TEST(ScalarConvertContract, PackedNarrowingKeepsTinyAndNormalLanesIndependent) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  const uint64_t Directed[] = {
      UINT64_C(0x36a0000000000000), UINT64_C(0x380fffffc0000000),
      UINT64_C(0x380fffffe0000000), UINT64_C(0x380fffffffffffff),
      UINT64_C(0x47efffffffffffff), UINT64_C(0x47f0000000000000),
      UINT64_C(0x7fefffffffffffff)};
  for (uint64_t Bits : Directed)
    for (unsigned Rounding = 0; Rounding < 4; ++Rounding)
      for (bool FTZ : {false, true})
        for (bool Swap : {false, true}) {
          const uint32_t State = 0x1f80 | (Rounding << 13) | (FTZ ? 0x8000 : 0);
          const uint64_t Normal = UINT64_C(0x3ff0000000000000);
          const uint64_t Input[] = {Swap ? Normal : Bits, Swap ? Bits : Normal};
          std::vector<uint8_t> Source(16);
          std::memcpy(Source.data(), Input, 16);
          const auto Tiny = nativeConvert(Bits, State, 2);
          const auto One = nativeConvert(Normal, State, 2);
          for (bool SAE : {false, true})
            for (unsigned Mask : {1U, 2U, 3U}) {
              SCOPED_TRACE(testing::Message()
                           << Bits << ':' << State << ':' << Swap << ':' << SAE
                           << ':' << Mask);
              NdOpEmulator E(Image);
              E.setStrictMode(true);
              E.setMXCSR(State);
              E.setRegisterBytes(1, Source);
              LowOp Op;
              Op.Opcode = NdOp::INTRINSIC;
              Op.Output = NdVar::tmp(2, 16);
              Op.addInput(NdVar::cst(unsigned(Intrinsic::X86FPConvert), 2));
              Op.addInput(NdVar::cst(
                  makeX86FPConvertControl(X86FPConvertKind::FloatToFloat, true,
                                          false, false, SAE,
                                          X86FPRounding::MXCSR, 2),
                  2));
              Op.addInput(NdVar::tmp(1, 16));
              Op.addInput(NdVar::cst(Mask, 1));
              ASSERT_TRUE(E.step(Op));
              ASSERT_TRUE(E.getRegisterBytes(2));
              const auto &Result = *E.getRegisterBytes(2);
              uint32_t ExpectedState = State;
              for (unsigned Lane = 0; Lane < 2; ++Lane) {
                const auto Expected = (Swap != bool(Lane)) ? One : Tiny;
                uint32_t Actual = 0;
                std::memcpy(&Actual, Result.data() + Lane * 4, 4);
                EXPECT_EQ(Actual, (Mask & (1U << Lane)) ? Expected.first : 0);
                if (!SAE && (Mask & (1U << Lane)))
                  ExpectedState |= Expected.second;
              }
              EXPECT_EQ(E.getMXCSR(), ExpectedState);
              for (unsigned Byte = 8; Byte < 16; ++Byte)
                EXPECT_EQ(Result[Byte], 0);
            }
        }
#else
  GTEST_SKIP() << "Native x64 instruction oracle requires GCC/Clang";
#endif
}

TEST(ScalarConvertContract,
     LowMedHighContractsRequireExactRolesAndNumericalSlice) {
  for (unsigned Control = 0; Control < 16; ++Control) {
    if (!x86FPScalarConvertControlIsValid(Control))
      continue;
    for (bool Memory : {false, true}) {
      const auto Op = scalarConvertOp(Control, 0x1f80, Memory);
      const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
      const auto Shape = x86FPStateLowShape(Op, Arch::X64);
      ASSERT_TRUE(x86FPStateShapeIsValid(Id, Shape));
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 0, Op.Output.Size - 4),
                Op.Output.Size - 4);
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 0, Op.Output.Size), 0U);
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, Op.Output.Size - 4, 4),
                0U);
      for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
        auto Bad = Op;
        switch (Mutation) {
        case 0:
          Bad.Inputs[2] = NdVar::tmp(9, 1);
          break;
        case 1:
          Bad.Inputs[2] = NdVar::cst(Control | 16, 1);
          break;
        case 2:
          Bad.Inputs[2].Size = 2;
          break;
        case 3:
          Bad.Inputs[3].Size = 8;
          break;
        case 4:
          Bad.Inputs[1].Size = Memory ? 4 : 16;
          break;
        case 5:
          Bad.Output.Size -= 4;
          break;
        case 6:
          Bad.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
          break;
        case 7:
          Bad.NumInputs = 3;
          break;
        case 8:
          Bad.Inputs[0].Size = 4;
          break;
        }
        EXPECT_FALSE(
            x86FPStateShapeIsValid(Id, x86FPStateLowShape(Bad, Arch::X64)));
      }
      MedOp Med;
      Med.Opcode = NdOp::INTRINSIC;
      Med.Output = {.Kind = MedVar::Temp,
                    .TheArch = Arch::X64,
                    .Id = 2,
                    .Size = Op.Output.Size};
      Med.addInput(MedVar::makeConst(unsigned(Id), 2));
      Med.addInput(MedVar::makeConst(0, Op.Inputs[1].Size));
      Med.addInput(MedVar::makeConst(Control, 1));
      Med.addInput(MedVar::makeConst(0x1f80, 4));
      ASSERT_TRUE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Med)));
      Med.Inputs[3].TheArch = Arch::ARM;
      EXPECT_FALSE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Med)));
      auto Call = HighExpr::makeCall("", 0,
                                     {HighExpr::makeConst(0, Op.Inputs[1].Size),
                                      HighExpr::makeConst(Control, 1),
                                      HighExpr::makeConst(0x1f80, 4)});
      Call->IntrinsicId = Id;
      Call->Type = NdType::makeInt(Op.Output.Size, false);
      ASSERT_TRUE(
          x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
      Call->Operands[0]->Type = NdType::makeFloat(Op.Inputs[1].Size);
      EXPECT_EQ(
          x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)),
          !Memory && !x86FPScalarConvertIsInteger(Control));
      Call->Operands[0]->Type = NdType::makeInt(Op.Inputs[1].Size, false);
      Call->IsIndirectCall = true;
      EXPECT_FALSE(
          x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
      EXPECT_EQ(x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X86)),
                !(x86FPScalarConvertIsInteger(Control) && (Control & 2)));
    }
  }
}

TEST(ScalarConvertContract,
     OwnedAssemblyAuthenticatesSeedSourceStateAndEffects) {
  for (unsigned Control : {2U, 3U, 4U, 7U, 10U, 15U})
    for (bool Memory : {false, true})
      for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
        SCOPED_TRACE(testing::Message()
                     << Control << ':' << Memory << ':' << Mutation);
        llvm::LLVMContext Context;
        llvm::Module Module("convert-owned", Context);
        llvm::IRBuilder<> B(Context);
        const auto Space = Memory ? NdMemoryAddressSpace::X86GS
                                  : NdMemoryAddressSpace::Default;
        const unsigned Layout = x86FPRoundStateLayout(
            x86FPScalarConvertSourceBytes(Control), Control, 0, Space);
        auto *Result = x86FPScalarConvertResultType(Context, Control);
        auto *Source = Memory ? static_cast<llvm::Type *>(B.getPtrTy(256))
                              : x86FPScalarConvertSourceType(Context, Control);
        if (Mutation == 1)
          Source = B.getInt16Ty();
        auto *State = B.getPtrTy(Mutation == 2 ? 257 : 0);
        auto *Fn = llvm::Function::Create(
            llvm::FunctionType::get(Result, {Source, State}, false),
            llvm::Function::ExternalLinkage, "convert", Module);
        B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Fn));
        auto *Type =
            llvm::FunctionType::get(Result, {Result, Source, State}, false);
        std::string Text = x86FPScalarConvertAsm(Layout, Memory);
        std::string Constraints =
            x86FPScalarConvertConstraints(Control, Memory);
        if (Mutation == 3)
          Text += "\n\tnop";
        if (Mutation == 4)
          Constraints.resize(Constraints.find(",~{memory}"));
        auto *Asm = llvm::InlineAsm::get(
            Type, Text, Constraints, Mutation != 5, Mutation == 6,
            Mutation == 7 ? llvm::InlineAsm::AD_Intel
                          : llvm::InlineAsm::AD_ATT);
        auto *Seed = llvm::ConstantFP::get(Result, Mutation == 8 ? -0.0 : 0.0);
        auto *Call = B.CreateCall(Asm, {Seed, Fn->getArg(0), Fn->getArg(1)});
        if (Mutation != 9)
          Call->setMetadata(X86FPStateAsmMetadata,
                            llvm::MDNode::get(Context, {}));
        B.CreateRet(Call);
        if (!Mutation)
          EXPECT_EQ(classifyX86FPStateAsm(*Call),
                    std::make_optional(std::pair{
                        Memory ? Intrinsic::X86FPScalarConvertMemoryState
                               : Intrinsic::X86FPScalarConvertState,
                        Layout}));
        else if (Mutation == 9)
          EXPECT_FALSE(classifyX86FPStateAsm(*Call));
        else
          EXPECT_DEATH(classifyX86FPStateAsm(*Call),
                       "invalid x86 FP state assembly");
      }
}

TEST(ScalarConvertContract, RawBytesAndDecodedMetadataMustAgree) {
  for (const auto &Form : convertForms())
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      SCOPED_TRACE(testing::Message() << formName(Form) << ':' << Mutation);
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Arch::X64));
      const bool Double =
          Form.Integer ? Form.DestinationBytes == 8 : Form.SourceBytes == 8;
      std::vector<uint8_t> Bytes;
      if (Form.Vex)
        Bytes = {0xc4, 0xe1,
                 uint8_t(0x70 | (Double ? 3 : 2) |
                         (Form.Integer && Form.SourceBytes == 8 ? 0x80 : 0))};
      else {
        Bytes.push_back(Double ? 0xf2 : 0xf3);
        if (Form.Integer && Form.SourceBytes == 8)
          Bytes.push_back(0x48);
        Bytes.push_back(0x0f);
      }
      Bytes.insert(Bytes.end(), {uint8_t(Form.Integer ? 0x2a : 0x5a),
                                 uint8_t(Form.Memory ? 0x40 : 0xc0)});
      if (Form.Memory)
        Bytes.push_back(16);
      DecodedInsn Insn;
      ASSERT_EQ(
          Decode.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Insn),
          int(Bytes.size()));
      auto &Detail = Insn.Raw->detail->x86;
      const unsigned Source = Form.Vex ? 2 : 1;
      switch (Mutation) {
      case 0:
        break;
      case 1:
        Detail.avx_sae = true;
        break;
      case 2:
        Detail.avx_rm = X86_AVX_RM_RD;
        break;
      case 3:
        Detail.operands[0].reg = X86_REG_XMM3;
        break;
      case 4:
        Detail.operands[Source].size = 32;
        break;
      case 5:
        Detail.encoding.imm_size = 1;
        break;
      case 6:
        Detail.op_count++;
        break;
      case 7:
        Insn.Raw->bytes[Detail.encoding.modrm_offset - 1] ^= 1;
        break;
      case 8:
        if (Form.Memory)
          Detail.operands[Source].mem.disp++;
        else
          Detail.operands[Source].reg =
              Form.Integer ? X86_REG_EBX : X86_REG_XMM3;
        break;
      case 9:
        Insn.Raw->size = 16;
        break;
      }
      std::vector<LowOp> Ops;
      if (Mutation) {
        EXPECT_THROW(Decode.liftToLow(Insn, Ops), UnliftedInstruction);
      } else {
        ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops));
        unsigned Found = 0;
        for (const auto &Op : Ops)
          if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
              isX86FPScalarConvertStateIntrinsic(
                  static_cast<Intrinsic>(Op.Inputs[0].Offset))) {
            ++Found;
            EXPECT_TRUE(x86FPStateShapeIsValid(
                static_cast<Intrinsic>(Op.Inputs[0].Offset),
                x86FPStateLowShape(Op, Arch::X64)));
          }
        EXPECT_EQ(Found, 1U);
      }
    }
}

TEST(ScalarConvertContract, MemoryReadsUseSourceWidthAndFailBeforeCompletion) {
  for (unsigned Control : {1U, 3U, 4U, 2U, 9U, 11U, 12U, 10U})
    for (auto Space :
         {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
          NdMemoryAddressSpace::X86GS}) {
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      const unsigned Bytes = x86FPScalarConvertSourceBytes(Control);
      Segment S;
      S.VA = 0x2001;
      S.Size = S.FileSz = Bytes;
      S.Flags = SegmentFlags::Readable;
      S.Data.resize(Bytes);
      Image.Segments.push_back(S);
      auto Op = scalarConvertOp(Control, 0x1f80, true);
      Op.MemoryAddressSpace = Space;
      const uint64_t Address =
          Space == NdMemoryAddressSpace::Default ? 0x2001 : 0x1001;
      for (unsigned Fault = 0; Fault < 4; ++Fault) {
        SCOPED_TRACE(testing::Message()
                     << Control << ':' << unsigned(Space) << ':' << Fault);
        Image.Segments[0].Flags =
            Fault == 1 ? SegmentFlags::Writable : SegmentFlags::Readable;
        Image.Segments[0].Size = Image.Segments[0].FileSz =
            Fault == 2 ? Bytes - 1 : Bytes;
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setX86LinearAddressBits(48);
        if (Space != NdMemoryAddressSpace::Default && Fault != 3)
          E.setMemoryAddressSpaceBase(Space, 0x1000);
        E.setRegister(1, Fault == 3 && Space == NdMemoryAddressSpace::Default
                             ? UINT64_C(0x800000000000)
                             : Address);
        E.setMXCSR(0x3f80);
        EXPECT_EQ(E.step(Op), Fault == 0);
        EXPECT_EQ(E.getMXCSR(),
                  Fault == 3 && Space != NdMemoryAddressSpace::Default
                      ? 0x3f80U
                      : 0x1f80U);
        EXPECT_EQ(bool(E.getRegisterBytes(2)), Fault == 0);
      }
    }
}

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
std::pair<uint64_t, uint32_t> nativeConvert(uint64_t Bits, uint32_t State,
                                            unsigned Control) {
  const unsigned Saved = _mm_getcsr();
  uint64_t Result = 0;
  // Constraint strings must be literal; integer and FP sources are distinct.
#define CONVERT_CASE(CONTROL, SOURCE, DESTINATION, ASM, CONSTRAINT)            \
  case CONTROL: {                                                              \
    SOURCE Source;                                                             \
    DESTINATION Destination;                                                   \
    std::memcpy(&Source, &Bits, sizeof(Source));                               \
    __asm__ volatile("ldmxcsr %1\n\t" ASM " %2,%0\n\tstmxcsr %1"               \
                     : "=&x"(Destination), "+m"(State)                         \
                     : CONSTRAINT(Source)                                      \
                     : "memory");                                              \
    std::memcpy(&Result, &Destination, sizeof(Destination));                   \
    break;                                                                     \
  }
  switch (Control & 7) {
    CONVERT_CASE(1, int32_t, float, "cvtsi2ssl", "r");
    CONVERT_CASE(3, int64_t, float, "cvtsi2ssq", "r");
    CONVERT_CASE(5, int32_t, double, "cvtsi2sdl", "r");
    CONVERT_CASE(7, int64_t, double, "cvtsi2sdq", "r");
    CONVERT_CASE(4, float, double, "cvtss2sd", "x");
    CONVERT_CASE(2, double, float, "cvtsd2ss", "x");
  }
#undef CONVERT_CASE
  _mm_setcsr(Saved);
  return {Result, State};
}
#endif

TEST(ScalarConvertContract, ConcreteBitsAndMxcsrMatchNative) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  uint64_t Random = UINT64_C(0x8fe3ab497351e02d);
  const uint64_t Directed[] = {0,
                               1,
                               UINT64_MAX,
                               UINT64_C(0x8000000000000000),
                               UINT64_C(0x1000001),
                               UINT64_C(0x20000000000001),
                               UINT64_C(0x7ff0000000000000),
                               UINT64_C(0x7ff0000000000071),
                               UINT64_C(0xfff8000040000000),
                               UINT64_C(0x36a0000000000000),
                               UINT64_C(0x7fefffffffffffff),
                               UINT64_C(0x3ff0000010000000),
                               UINT64_C(0xbff0000010000000),
                               UINT64_C(0x47efffffe0000000),
                               UINT64_C(0x47efffffe0000001),
                               UINT64_C(0x47efffffffffffff),
                               UINT64_C(0x47f0000000000000),
                               UINT64_C(0xc7efffffffffffff),
                               UINT64_C(0xc7f0000000000000),
                               UINT64_C(0x3810000000000000),
                               UINT64_C(0x380fffffffffffff),
                               UINT64_C(0x380fffffc0000000),
                               UINT64_C(0x380fffffe0000000),
                               0x7f800071,
                               0xffc00077,
                               0x007fffff,
                               0x80000000,
                               0x3f800001};
  for (unsigned Control = 0; Control < 16; ++Control) {
    if (!x86FPScalarConvertControlIsValid(Control))
      continue;
    const unsigned SourceBytes = x86FPScalarConvertSourceBytes(Control);
    const unsigned Bytes = x86FPScalarConvertDestinationBytes(Control);
    for (unsigned Environment = 0; Environment < 32; ++Environment)
      for (unsigned I = 0; I < 256; ++I) {
        Random ^= Random << 13;
        Random ^= Random >> 7;
        Random ^= Random << 17;
        const uint64_t Bits = I < std::size(Directed) ? Directed[I] : Random;
        const uint32_t State = 0x1f80 | ((Environment & 3) << 13) |
                               ((Environment & 4) ? 0x40 : 0) |
                               ((Environment & 8) ? 0x8000 : 0) |
                               ((Environment & 16) ? 0x25 : 0);
        const auto Expected = nativeConvert(Bits, State, Control);
        SCOPED_TRACE(testing::Message()
                     << Control << ':' << Environment << ':' << I);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(1, Bits);
        LowOp Op;
        Op.Opcode = NdOp::INTRINSIC;
        Op.Output = NdVar::tmp(2, Bytes + 4);
        Op.addInput(
            NdVar::cst(unsigned(Intrinsic::X86FPScalarConvertState), 2));
        Op.addInput(NdVar::tmp(1, SourceBytes));
        Op.addInput(NdVar::cst(Control, 1));
        Op.addInput(NdVar::cst(State, 4));
        ASSERT_TRUE(E.step(Op));
        const auto Result = E.getRegisterBytes(2);
        ASSERT_TRUE(Result);
        ASSERT_EQ(Result->size(), Bytes + 4);
        uint64_t Number = 0;
        uint32_t Outgoing = 0;
        std::memcpy(&Number, Result->data(), Bytes);
        std::memcpy(&Outgoing, Result->data() + Bytes, 4);
        ASSERT_EQ(Number, Expected.first);
        ASSERT_EQ(Outgoing, Expected.second);
        ASSERT_EQ(E.getMXCSR(), Expected.second);
      }
  }
#else
  GTEST_SKIP() << "Native x64 instruction oracle requires GCC/Clang";
#endif
}

class ScalarConvertFixture : public X86FPStateFixture {
protected:
  ConvertForm Form{};
  bool KeepResult = true;
  unsigned SourceOffset = 64;
  unsigned FaultMode = 0;
  unsigned SourceRegister = 2;
  NdMemoryAddressSpace SourceSegment = NdMemoryAddressSpace::Default;

  std::string conversionFaultDriver(bool Declare, bool NativeCall) {
    const bool MemoryFault = FaultMode == 5;
    const unsigned State = FaultMode == 0   ? 0x0f80
                           : FaultMode == 1 ? 0x1f00
                           : FaultMode == 2 ? 0x1e80
                           : FaultMode == 3 ? 0x1780
                                            : 0x1f80;
    const unsigned Raised = FaultMode == 0   ? 0x20
                            : FaultMode == 1 ? 1
                            : FaultMode == 2 ? 2
                            : FaultMode == 3 ? 0x10
                                             : 0;
    const uint64_t Bits =
        FaultMode == 0 ? UINT64_C(0x1000001)
        : FaultMode == 1
            ? (Form.SourceBytes == 4 ? UINT64_C(0x7f800071)
                                     : UINT64_C(0x7ff0000000000071))
        : FaultMode == 2 ? 1
                         : UINT64_C(0x36a0000000000000);
    std::string Text =
        "#define _GNU_SOURCE\n#include <stdint.h>\n"
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <immintrin.h>\nextern void native_probe(uintptr_t);\n";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += "#define MEMORY_FAULT " + std::to_string(MemoryFault) +
            "\n#define SOURCE_OFFSET " + std::to_string(SourceOffset) +
            "\n#define SOURCE_BYTES " + std::to_string(Form.SourceBytes) +
            "\n#define EXPECTED_STATE " + std::to_string(State | Raised) +
            "u\n";
    Text += R"(
static unsigned char *actual;
static unsigned char before[160];
static void check(unsigned state, uintptr_t address) {
  if (state != EXPECTED_STATE) {
    fprintf(stderr, "exception MXCSR expected=%08x actual=%08x\n", EXPECTED_STATE, state); _Exit(2);
  }
  if (MEMORY_FAULT && (address < (uintptr_t)actual + SOURCE_OFFSET ||
      address >= (uintptr_t)actual + SOURCE_OFFSET + SOURCE_BYTES)) _Exit(3);
  if (memcmp(actual, before, MEMORY_FAULT ? 64 : 160)) _Exit(4);
  _Exit(0);
}
#ifdef _WIN32
#include <windows.h>
static LONG CALLBACK observe(EXCEPTION_POINTERS *info) {
  DWORD code = info->ExceptionRecord->ExceptionCode;
  if (MEMORY_FAULT ? code != EXCEPTION_ACCESS_VIOLATION
      : code != EXCEPTION_FLT_INVALID_OPERATION && code != EXCEPTION_FLT_INEXACT_RESULT &&
        code != EXCEPTION_FLT_UNDERFLOW && code != EXCEPTION_FLT_DENORMAL_OPERAND)
    return EXCEPTION_CONTINUE_SEARCH;
  check(info->ContextRecord->MxCsr, MEMORY_FAULT ?
        (uintptr_t)info->ExceptionRecord->ExceptionInformation[1] : 0);
  return EXCEPTION_CONTINUE_EXECUTION;
}
#else
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>
static void observe(int signal, siginfo_t *info, void *context) {
  if (signal != (MEMORY_FAULT ? SIGSEGV : SIGFPE)) _Exit(5);
  ucontext_t *uc = (ucontext_t *)context;
  check(uc->uc_mcontext.fpregs->mxcsr, (uintptr_t)info->si_addr);
}
#endif
int main(void) {
  _Alignas(64) unsigned char storage[160];
  actual = storage;
#ifdef _WIN32
  if (!AddVectoredExceptionHandler(1, observe)) return 10;
  if (MEMORY_FAULT) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    unsigned char *base = VirtualAlloc(0, 2*si.dwPageSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    DWORD previous;
    if (!base || !VirtualProtect(base + si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &previous)) return 11;
    actual = base + si.dwPageSize - 64;
  }
#else
  struct sigaction action = {0}; action.sa_sigaction = observe; action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(MEMORY_FAULT ? SIGSEGV : SIGFPE, &action, 0)) return 10;
  if (MEMORY_FAULT) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *base = mmap(0, 2*page, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED || mprotect(base + page, page, PROT_NONE)) return 11;
    actual = base + page - 64;
  }
#endif
  for (unsigned i = 0; i < (MEMORY_FAULT ? 64u : 160u); ++i)
    actual[i] = before[i] = (unsigned char)(0x59u + i*37u);
)";
    if (!MemoryFault)
      Text += "  uint64_t bits = " + std::to_string(Bits) +
              "ULL;\n"
              "  memcpy(actual + SOURCE_OFFSET, &bits, SOURCE_BYTES);\n"
              "  memcpy(before, actual, 160);\n";
    Text += "  _mm_setcsr(" + std::to_string(State) + ");\n";
    Text += NativeCall ? "  native_probe((uintptr_t)actual);\n"
                       : "  isa_probe((uintptr_t)actual);\n";
    return Text + "  return 99; /* required exception did not occur */\n}\n";
  }

  std::vector<uint8_t> kernel() {
    const uint8_t Base = memoryModRM();
    // Preserve distinct destination/source1 upper bytes, including upper YMM.
    std::vector<uint8_t> Bytes{
        0xc5, 0xfe, 0x6f, Base, 0xc5, 0xfe, 0x6f, uint8_t(0x48 | Base), 32};
    if (!Form.Memory) {
      if (Form.Integer) {
        if (Form.SourceBytes == 8)
          Bytes.push_back(0x48);
        Bytes.insert(Bytes.end(),
                     {0x8b, uint8_t(0x40 | Base), uint8_t(SourceOffset)});
      } else {
        Bytes.insert(Bytes.end(), {0xc5, 0xfe, 0x6f,
                                   uint8_t(0x40 | (SourceRegister << 3) | Base),
                                   uint8_t(SourceOffset)});
      }
    }
    const bool PrefixDouble =
        Form.Integer ? Form.DestinationBytes == 8 : Form.SourceBytes == 8;
    if (SourceSegment != NdMemoryAddressSpace::Default)
      Bytes.push_back(0x65);
    if (Form.Vex) {
      Bytes.insert(
          Bytes.end(),
          {0xc4, 0xe1,
           uint8_t(0x70 | (PrefixDouble ? 3 : 2) |
                   (Form.Integer && Form.SourceBytes == 8 ? 0x80 : 0))});
    } else {
      Bytes.push_back(PrefixDouble ? 0xf2 : 0xf3);
      if (Form.Integer && Form.SourceBytes == 8)
        Bytes.push_back(0x48);
      Bytes.push_back(0x0f);
    }
    Bytes.push_back(Form.Integer ? 0x2a : 0x5a);
    if (Form.Memory && SourceSegment != NdMemoryAddressSpace::Default) {
#if defined(_WIN32)
      const uint8_t Offset =
          0x30; // TEB self pointer: an actual native GS source.
#else
      const uint8_t Offset = 64;
#endif
      Bytes.insert(Bytes.end(), {0x04, 0x25, Offset, 0, 0, 0});
    } else if (Form.Memory)
      Bytes.insert(Bytes.end(), {uint8_t(0x40 | Base), uint8_t(SourceOffset)});
    else
      Bytes.push_back(Form.Integer ? 0xc0 : uint8_t(0xc0 | SourceRegister));
    if (KeepResult)
      Bytes.insert(Bytes.end(), {0xc5, 0xfe, 0x7f, uint8_t(0x40 | Base), 96});
    Bytes.insert(Bytes.end(),
                 {0x0f, 0xae, uint8_t(0x98 | Base), 128, 0, 0, 0, 0xc3});
    return Bytes;
  }

  std::string comparisonDriver(bool, bool Declare, bool Fault,
                               bool NativeCall = false) override {
    if (Fault)
      return conversionFaultDriver(Declare, NativeCall);
    std::string Text = R"(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <immintrin.h>
extern void native_probe(uintptr_t);
)";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += "static const uint64_t values[] = {";
    if (Form.Integer)
      Text += R"(
  0, 1, UINT64_MAX, INT32_MAX, UINT64_C(0x80000000),
  UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000),
  UINT64_C(0x1000001), UINT64_C(0xfffffffffeffffff),
  UINT64_C(0x20000000000001), UINT64_C(0xffdfffffffffffff),
  UINT64_C(0x8000000000000401)
)";
    else if (Form.SourceBytes == 8)
      Text += R"(
  0, UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000),
  UINT64_C(0x3ff0000010000000), UINT64_C(0xbff0000010000000),
  1, UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000),
  UINT64_C(0x36a0000000000000), UINT64_C(0x380fffffc0000000),
  UINT64_C(0x7fefffffffffffff), UINT64_C(0x7ff0000000000000),
  UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000000031),
  UINT64_C(0xfff8000040000000), UINT64_C(0x7ff0000000000071)
)";
    else
      Text += R"(
  0, 0x80000000, 0x3f800000, 0xbf800000, 0x3f800001, 0x3dcccccd,
  1, 0x007fffff, 0x00800000, 0x7f7fffff, 0x7f800000, 0xff800000,
  0x7fc00031, 0xffc00077, 0x7f800071
)";
    Text += R"(
};
int main(void) {
  const unsigned saved = _mm_getcsr();
  for (unsigned rounding = 0; rounding < 4; ++rounding)
    for (unsigned environment = 0; environment < 4; ++environment)
      for (unsigned sticky = 0; sticky < 2; ++sticky)
        for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
          _Alignas(64) unsigned char expected[160], actual[160];
          for (unsigned j = 0; j < sizeof(expected); ++j)
            expected[j] = (unsigned char)(0x59u + j * 37u);
)";
    Text += "          memcpy(expected + " + std::to_string(SourceOffset) +
            ", &values[i], " + std::to_string(Form.SourceBytes) + ");\n";
    Text += R"(
          memcpy(actual, expected, sizeof(expected));
          const unsigned state = 0x1f80 | (rounding << 13) |
            ((environment & 1) ? 0x40 : 0) |
            ((environment & 2) ? 0x8000 : 0) | (sticky ? 0x25 : 0);
          _mm_setcsr(state); native_probe((uintptr_t)expected);
          unsigned expected_state = _mm_getcsr();
          _mm_setcsr(state); isa_probe((uintptr_t)actual);
          unsigned actual_state = _mm_getcsr(); _mm_setcsr(saved);
          if (expected_state != actual_state || memcmp(expected, actual, sizeof(expected))) {
            printf("index=%u input=%016llx incoming=%08x expected=%08x actual=%08x\n",
                   i, (unsigned long long)values[i], state, expected_state, actual_state);
            for (unsigned j = 0; j < sizeof(expected); ++j)
              if (expected[j] != actual[j]) printf("byte%u expected=%02x actual=%02x\n", j, expected[j], actual[j]);
            return 1;
          }
        }
  return 0;
}
)";
#if defined(__linux__)
    if (SourceSegment == NdMemoryAddressSpace::X86GS) {
      const auto Replace = [&](const std::string &Old, const std::string &New) {
        const auto Position = Text.find(Old);
        EXPECT_NE(Position, std::string::npos);
        if (Position != std::string::npos)
          Text.replace(Position, Old.size(), New);
      };
      Replace("#include <stdint.h>",
              "#include <stdint.h>\n#include <unistd.h>\n#include "
              "<sys/syscall.h>\n#include <asm/prctl.h>");
      Replace(
          "const unsigned saved = _mm_getcsr();",
          "const unsigned saved = _mm_getcsr(); unsigned long saved_gs;\n"
          "if (syscall(SYS_arch_prctl, ARCH_GET_GS, &saved_gs)) return 77;");
      Replace("native_probe((uintptr_t)expected);",
              "if (syscall(SYS_arch_prctl, ARCH_SET_GS, expected)) return 77;\n"
              "native_probe((uintptr_t)expected);");
      Replace("isa_probe((uintptr_t)actual);",
              "if (syscall(SYS_arch_prctl, ARCH_SET_GS, actual)) return 77;\n"
              "isa_probe((uintptr_t)actual);");
      Replace("unsigned actual_state = _mm_getcsr();",
              "unsigned actual_state = _mm_getcsr();\n"
              "if (syscall(SYS_arch_prctl, ARCH_SET_GS, saved_gs)) return 77;");
    }
#endif
    return Text;
  }
};

class ScalarConvertForms : public ScalarConvertFixture,
                           public testing::WithParamInterface<ConvertForm> {};

TEST_P(ScalarConvertForms, NativeCompletionMatchesEveryRoute) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "The full YMM observer requires an AVX-enabled x64 host";
  Form = GetParam();
  compareFloating(kernel(), Form.DestinationBytes == 8);
}

INSTANTIATE_TEST_SUITE_P(LegacyAndVex, ScalarConvertForms,
                         testing::ValuesIn(convertForms()),
                         [](const testing::TestParamInfo<ConvertForm> &Info) {
                           return formName(Info.param);
                         });

TEST_F(ScalarConvertFixture, DiscardedIntegerConversionStillRaisesPrecision) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "The full YMM observer requires an AVX-enabled x64 host";
  Form = {true, 8, 4, true, true};
  KeepResult = false;
  compareFloating(kernel(), false);
}

TEST_F(ScalarConvertFixture, UnalignedIntegerAndPrecisionMemorySources) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  SourceOffset = 65;
  for (auto F : {ConvertForm{true, 8, 4, false, true},
                 ConvertForm{false, 4, 8, true, true}}) {
    Form = F;
    compareFloating(kernel(), Form.DestinationBytes == 8);
  }
}

TEST_F(ScalarConvertFixture, PrecisionSourceCanAliasDestinationOrVexMerge) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  for (bool Vex : {false, true})
    for (unsigned Source : {0U, 1U}) {
      Form = {false, 8, 4, Vex, false};
      SourceRegister = Source;
      compareFloating(kernel(), false);
    }
}

TEST_F(ScalarConvertFixture, GSIntegerAndPrecisionSourcesRetainAddressSpace) {
#if defined(_WIN32) || defined(__linux__)
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  SourceSegment = NdMemoryAddressSpace::X86GS;
  for (auto F : {ConvertForm{true, 8, 4, true, true},
                 ConvertForm{false, 8, 4, false, true}}) {
    Form = F;
    compareFloating(kernel(), false);
  }
#else
  GTEST_SKIP() << "Native GS oracle requires Windows/Linux x64";
#endif
}

TEST_F(ScalarConvertFixture, DiscardedPrecisionConversionStillRaisesInvalid) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "The full YMM observer requires an AVX-enabled x64 host";
  Form = {false, 8, 4, false, true};
  KeepResult = false;
  compareFloating(kernel(), false);
}

TEST_F(ScalarConvertFixture, UnmaskedPrecisionCommitsNoResult) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  Form = {true, 8, 4, true, false};
  FaultMode = 0;
  compareFloating(kernel(), false, true);
}
TEST_F(ScalarConvertFixture, UnmaskedInvalidCommitsNoResult) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  Form = {false, 4, 8, true, true};
  FaultMode = 1;
  compareFloating(kernel(), true, true);
}
TEST_F(ScalarConvertFixture, UnmaskedDenormalPrecedesUnderflowAndPrecision) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  Form = {false, 8, 4, false, true};
  FaultMode = 2;
  compareFloating(kernel(), false, true);
}
TEST_F(ScalarConvertFixture, ExactTinyResultStillTrapsWhenUnderflowIsUnmasked) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  Form = {false, 8, 4, true, true};
  FaultMode = 3;
  compareFloating(kernel(), false, true);
}
TEST_F(ScalarConvertFixture, DiscardedMemorySourceRetainsTheFullFaultingRead) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  for (auto F : {ConvertForm{true, 4, 4, false, true},
                 ConvertForm{false, 8, 4, true, true}}) {
    Form = F;
    FaultMode = 5;
    KeepResult = false;
    SourceOffset = 63;
    compareFloating(kernel(), false, true);
  }
}
class ScalarConvertReturnAccuracy : public X86FPStateFixture {
protected:
  void compareFloatingReturn(bool IsDouble, bool Vex) {
    if (!nativeX64())
      GTEST_SKIP() << "native x86_64 host required";
    if (Vex && !llvm::sys::getHostCPUFeatures().lookup("avx"))
      GTEST_SKIP() << "native AVX required for the VEX scalar return probe";
    const std::vector<uint8_t> Bytes =
        Vex ? std::vector<uint8_t>{0xc5,
                                   static_cast<uint8_t>(IsDouble ? 0xfa : 0xfb),
                                   0x5a, 0xc0, 0xc3}
            : std::vector<uint8_t>{static_cast<uint8_t>(IsDouble ? 0xf3 : 0xf2),
                                   0x0f, 0x5a, 0xc0, 0xc3};
    const auto Native = file("return-native.s");
    const auto NativeObject = file("return-native.o");
    auto Assembly = assembly(Bytes);
    for (size_t Position = 0;
         (Position = Assembly.find("isa_probe", Position)) !=
         std::string::npos;)
      Assembly.replace(Position, 9, "native_probe");
    write(Native, Assembly);
    auto Built = command(Compiler, {"-c", Native, "-o", NativeObject});
    ASSERT_EQ(Built.Status, 0) << Built.Error;
    const std::string Scalar = IsDouble ? "double" : "float";
    const std::string SourceScalar = IsDouble ? "float" : "double";
    const auto Common = floatingDriver(!IsDouble, false);
    const auto Values = Common.find("static const");
    const auto Main = Common.find("int main(void)");
    ASSERT_NE(Values, std::string::npos);
    ASSERT_NE(Main, std::string::npos);
    std::string Driver = "\n#include <stdint.h>\n#include <string.h>\n"
                         "#include <immintrin.h>\nextern " +
                         Scalar + " native_probe(" + SourceScalar + ");\n" +
                         Common.substr(Values, Main - Values) +
                         "\nint main(void) {\nuint32_t saved = _mm_getcsr();\n";
    Driver += R"(
for (unsigned rounding = 0; rounding < 4; ++rounding)
  for (unsigned environment = 0; environment < 4; ++environment)
    for (unsigned sticky = 0; sticky < 2; ++sticky)
      for (unsigned a = 0; a < sizeof(values)/sizeof(values[0]); ++a) {
        uint32_t state = 0x1f80 | (rounding << 13) |
          ((environment & 1) ? 0x40 : 0) | ((environment & 2) ? 0x8000 : 0) |
          (sticky ? 0x25 : 0);
)";
    Driver += SourceScalar +
              " input; __builtin_memcpy(&input, &values[a], sizeof(input));\n"
              "_mm_setcsr(state);\n" +
              Scalar +
              " expected = native_probe(input);\n"
              "uint32_t expected_state = _mm_getcsr();\n"
              "_mm_setcsr(state);\n" +
              Scalar +
              " actual = isa_probe(input);\n"
              "uint32_t actual_state = _mm_getcsr();\n_mm_setcsr(saved);\n"
              "if (expected_state != actual_state || memcmp(&expected, "
              "&actual, sizeof(actual))) return 1;\n"
              "}\nreturn 0;\n}\n";
    for (bool NoOpt : {false, true})
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM ? "LLVMC return" : "HighC return");
        SCOPED_TRACE(NoOpt);
        llvm::LLVMContext Context;
        auto Image = image(Bytes);
        PipelineOptions Options;
        Options.LiftMode = LLVM;
        Options.SourceProjection = LLVM;
        Options.NoOpt = NoOpt;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Entry};
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string DebugIR;
        llvm::raw_string_ostream DebugOut(DebugIR);
        Pipeline::dumpLowIR(Result.LowFuncs, DebugOut);
        Pipeline::dumpMedIR(Result.MedFuncs, DebugOut);
        retain(std::string(LLVM ? "llvm" : "high") +
                   (NoOpt ? "-noopt-return.ir" : "-return.ir"),
               DebugIR);
        CEmitterOptions Emission;
        Emission.TheArch = Arch::X64;
        Emission.Format = hostFormat();
        Emission.PreserveLLVMFunctionTypes = true;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVM) {
          ASSERT_NE(Result.LlvmModule, nullptr);
          ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, Emission,
                                          nullptr, &Image));
        } else {
          ASSERT_EQ(Result.HighFuncs.size(), 1U);
          ASSERT_NE(Result.HighFuncs[0].ReturnType, nullptr);
          ASSERT_EQ(Result.HighFuncs[0].ReturnType->Kind, NdTypeKind::Float);
          ASSERT_EQ(Result.HighFuncs[0].ReturnType->Size, IsDouble ? 8 : 4);
          ASSERT_EQ(Result.HighFuncs[0].Params.size(), 1U);
          ASSERT_EQ(Result.HighFuncs[0].Params[0].Type->Kind, NdTypeKind::Float)
              << DebugIR;
          ASSERT_EQ(Result.HighFuncs[0].Params[0].Type->Size, IsDouble ? 4 : 8);
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emission));
        }
        const auto C = file("return-driver.c");
        std::string TypedDriver = Driver;
        if (LLVM) {
          const auto *Function = Result.LlvmModule->getFunction("isa_probe");
          ASSERT_NE(Function, nullptr);
          ASSERT_EQ(Function->arg_size(), 1U);
          // PreserveLLVMFunctionTypes exposes the complete XMM carrier. Feed
          // and observe its low scalar bytes through that actual C signature.
          if (Function->getArg(0)->getType()->isVectorTy()) {
            ASSERT_TRUE(Function->getReturnType()->isVectorTy());
            const std::string Call = Scalar + " actual = isa_probe(input);";
            const auto Position = TypedDriver.find(Call);
            ASSERT_NE(Position, std::string::npos);
            TypedDriver.replace(
                Position, Call.size(),
                "uint64_t __attribute__((vector_size(16))) incoming = {0,0};\n"
                "__builtin_memcpy(&incoming, &input, sizeof(input));\n"
                "uint64_t __attribute__((vector_size(16))) returned = "
                "isa_probe(incoming);\n" +
                    Scalar +
                    " actual; __builtin_memcpy(&actual, &returned, "
                    "sizeof(actual));");
          }
        }
        write(C, Source + TypedDriver);
        for (const char *Optimization : {"-O0", "-O2"}) {
          const auto Executable = file("return.exe");
          Built = command(Compiler,
                          {Optimization, NativeObject, C, "-o", Executable});
          ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
          const auto Actual = command(Executable, {});
          EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error << Source;
        }
      }
  }
};

TEST_F(ScalarConvertReturnAccuracy,
       PrecisionConversionsRetainTypedReturnAndMxcsr) {
  for (bool IsDouble : {false, true})
    for (bool Vex : {false, true})
      compareFloatingReturn(IsDouble, Vex);
}

class ScalarConvertStackAccuracy : public X86FPStateFixture {
protected:
  void compareStackArguments(bool IsDouble, bool Declared) {
    if (!nativeX64())
      GTEST_SKIP() << "native x86_64 host required";
    const unsigned Registers = hostFormat() == BinaryFormat::COFF ? 4 : 8;
    const unsigned Offset = hostFormat() == BinaryFormat::COFF ? 40 : 8;
    const std::string Scalar = IsDouble ? "double" : "float";
    const std::string ReturnScalar = IsDouble ? "float" : "double";
    const std::vector<uint8_t> Bytes = {
        static_cast<uint8_t>(IsDouble ? 0xf2 : 0xf3),
        0x0f,
        0x5a,
        0x44,
        0x24,
        static_cast<uint8_t>(Offset),
        0xc3};
    const auto Native = file("stack-native.s");
    const auto NativeObject = file("stack-native.o");
    auto NativeText = assembly(Bytes);
    for (size_t Position = 0;
         (Position = NativeText.find("isa_probe", Position)) !=
         std::string::npos;)
      NativeText.replace(Position, 9, "native_probe");
    write(Native, NativeText);
    auto Built = command(Compiler, {"-c", Native, "-o", NativeObject});
    ASSERT_EQ(Built.Status, 0) << Built.Error;
    std::string Signature, Arguments;
    for (unsigned I = 0; I <= Registers; ++I) {
      if (I) {
        Signature += ',';
        Arguments += ',';
      }
      Signature += Scalar;
      Arguments += I == 0 ? "left" : I == Registers ? "right" : "zero";
    }
    const auto Common = floatingDriver(IsDouble, false);
    const auto Values = Common.find("static const");
    const auto Main = Common.find("int main(void)");
    std::string Driver =
        "\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n"
        "#include <immintrin.h>\nextern " +
        ReturnScalar + " native_probe(" + Signature + ");\n" +
        Common.substr(Values, Main - Values) + R"(
int main(void) {
  uint32_t saved = _mm_getcsr();
  for (unsigned rounding = 0; rounding < 4; ++rounding)
    for (unsigned environment = 0; environment < 4; ++environment)
      for (unsigned sticky = 0; sticky < 2; ++sticky)
        for (unsigned a = 0; a < sizeof(values)/sizeof(values[0]); ++a)
          for (unsigned b = 0; b < sizeof(values)/sizeof(values[0]); ++b) {
)";
    Driver += Scalar +
              " left, right, zero = 0;\n"
              "memcpy(&left, &values[a], sizeof(left));\n"
              "memcpy(&right, &values[b], sizeof(right));\n"
              "uint32_t state = 0x1f80 | (rounding << 13) | "
              "((environment & 1) ? 0x40 : 0) | "
              "((environment & 2) ? 0x8000 : 0) | (sticky ? 0x25 : 0);\n"
              "_mm_setcsr(state);\n" +
              ReturnScalar + " expected = native_probe(" + Arguments +
              ");\nuint32_t expected_state = _mm_getcsr();\n"
              "_mm_setcsr(state);\n" +
              ReturnScalar + " actual = isa_probe(" + Arguments + R"();
uint32_t actual_state = _mm_getcsr(); _mm_setcsr(saved);
if (memcmp(&actual, &expected, sizeof(actual)) || actual_state != expected_state) {
  printf("stack argument mismatch a=%u b=%u state=%08x expected=%08x actual=%08x\n",
         a, b, state, expected_state, actual_state);
  return 1;
}
}
return 0;
}
)";
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(NoOpt);
      auto Image = image(Bytes);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.NoOpt = NoOpt;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Entry};
      if (Declared) {
        SourceFunctionTypeHint Hint;
        Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
        Hint.Architecture = Arch::X64;
        Hint.HasExplicitABI = true;
        Hint.ReturnType = NdType::makeFloat(IsDouble ? 4 : 8);
        const auto &TRI = getTargetRegInfo(Arch::X64);
        for (unsigned I = 0; I <= Registers; ++I) {
          SourceParameterTypeHint Parameter;
          Parameter.Name = "arg" + std::to_string(I);
          Parameter.Type = NdType::makeFloat(IsDouble ? 8 : 4);
          Parameter.Location.Kind =
              I == Registers ? SourceABICarrierKind::Stack
                             : SourceABICarrierKind::FloatingRegister;
          Parameter.Location.RegisterOffset =
              I == Registers ? 0 : TRI.FPParamRegs[I];
          Parameter.Location.EntryStackOffset = I == Registers ? Offset : 0;
          Parameter.Location.ValueBytes = IsDouble ? 8 : 4;
          Hint.Parameters.push_back(Parameter);
        }
        Hint.ReturnLocation.Kind = SourceABICarrierKind::FloatingRegister;
        Hint.ReturnLocation.RegisterOffset = TRI.FPReturnReg;
        Hint.ReturnLocation.ValueBytes = IsDouble ? 4 : 8;
        Options.SourceTypeHints[Entry] = Hint;
      }
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.HighFuncs.size(), 1u);
      const auto &Function = Result.HighFuncs[0];
      ASSERT_EQ(Function.Params.size(), Registers + 1);
      if (Declared) {
        ASSERT_EQ(Result.MedFuncs.size(), 1u);
        ASSERT_TRUE(Result.MedFuncs[0].SourceParametersBound);
        ASSERT_TRUE(Function.SourceTypeHint);
      }
      for (const auto &Parameter : Function.Params) {
        ASSERT_NE(Parameter.Type, nullptr);
        EXPECT_EQ(Parameter.Type->Kind, NdTypeKind::Float);
        EXPECT_EQ(Parameter.Type->Size, IsDouble ? 8 : 4);
      }
      CEmitterOptions Emission;
      Emission.TheArch = Arch::X64;
      Emission.Format = hostFormat();
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emission));
      retain(NoOpt ? "stack-noopt.c" : "stack-default.c", Source);
      const auto C = file("stack-driver.c");
      write(C, Source + Driver);
      for (const char *Optimization : {"-O0", "-O2"}) {
        const auto Executable =
            file(std::string("stack") + Optimization + ".exe");
        Built = command(Compiler,
                        {Optimization, NativeObject, C, "-o", Executable});
        ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
        const auto Actual = command(Executable, {});
        EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error << Source;
      }
    }
  }
};

TEST_F(ScalarConvertStackAccuracy,
       DeclaredPrecisionSourceUsesStackSourceWidth) {
  compareStackArguments(false, true);
  compareStackArguments(true, true);
}

} // namespace
