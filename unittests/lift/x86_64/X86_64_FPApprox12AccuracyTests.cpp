//===- X86_64_FPApprox12AccuracyTests.cpp - RCP/RSQRT semantics tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/ir/high/X86FPStateShape.h"

#include <cmath>

namespace {
LowOp approxOp(unsigned Control, unsigned Bytes, bool Memory = false,
               uint64_t Address = 0x2000,
               NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.MemoryAddressSpace = Space;
  Op.Output = NdVar::tmp(0, Bytes);
  Op.addInput(NdVar::cst(unsigned(Memory ? Intrinsic::X86FPApprox12MemoryState
                                         : Intrinsic::X86FPApprox12State),
                         2));
  if (Memory) {
    Op.addInput(NdVar::cst(Address, 8));
    Op.addInput(NdVar::cst(Control, 1));
  } else {
    Op.addInput(NdVar::cst(Control, 1));
    Op.addInput(NdVar::reg(x86reg::XMM0, Bytes));
  }
  return Op;
}

void addApproxMemory(BinaryImage &Image, uint64_t Address, unsigned Bytes,
                     bool Readable = true) {
  Segment Memory;
  Memory.VA = Address;
  Memory.Size = Memory.FileSz = Bytes;
  Memory.Flags = Readable ? SegmentFlags::Readable : SegmentFlags::Writable;
  Memory.Data.resize(Bytes, 0);
  Image.Segments.push_back(Memory);
}

void checkApproxEnvelope(uint32_t Input, uint32_t Output, bool Rsqrt) {
  const uint32_t Magnitude = Input & 0x7fffffffU;
  const uint32_t Sign = Input & 0x80000000U;
  if (Magnitude > 0x7f800000) {
    EXPECT_EQ(Output, Input | 0x00400000U);
    return;
  }
  if (Magnitude < 0x00800000) {
    EXPECT_EQ(Output, Sign | 0x7f800000U);
    return;
  }
  if (Rsqrt && Sign) {
    EXPECT_EQ(Output, 0xffc00000U);
    return;
  }
  if (Magnitude == 0x7f800000) {
    EXPECT_EQ(Output, Sign);
    return;
  }
  if (!Rsqrt && Magnitude >= 0x7e800c01U) {
    EXPECT_EQ(Output, Sign);
    return;
  }
  // In the documented implementation-dependent tininess interval, a zero
  // result is permitted. Everywhere else check against a double oracle.
  if (!Rsqrt && Magnitude > 0x7e7fe800U && Output == Sign)
    return;
  EXPECT_EQ(Output & 0x80000000U, Sign);
  EXPECT_NE(Output & 0x7f800000U, 0U);
  EXPECT_NE(Output & 0x7f800000U, 0x7f800000U);
  float Source, Result;
  std::memcpy(&Source, &Input, 4);
  std::memcpy(&Result, &Output, 4);
  const double Exact =
      Rsqrt ? 1.0 / std::sqrt(double(Source)) : 1.0 / double(Source);
  EXPECT_LE(std::abs((double(Result) - Exact) / Exact), 1.5 / 4096.0);
}

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
uint32_t nativeApprox(uint32_t Bits, bool Rsqrt, uint32_t &State) {
  float Source, Result;
  std::memcpy(&Source, &Bits, 4);
  const auto Saved = _mm_getcsr();
  if (Rsqrt)
    __asm__ volatile("ldmxcsr %1\n\trsqrtss %2,%0\n\tstmxcsr %1"
                     : "=&x"(Result), "+m"(State)
                     : "x"(Source)
                     : "memory");
  else
    __asm__ volatile("ldmxcsr %1\n\trcpss %2,%0\n\tstmxcsr %1"
                     : "=&x"(Result), "+m"(State)
                     : "x"(Source)
                     : "memory");
  _mm_setcsr(Saved);
  std::memcpy(&Bits, &Result, 4);
  return Bits;
}
#endif

TEST(X86FPApprox12Contract, RepresentativeAndNativeRespectNumericalEnvelope) {
  std::vector<uint32_t> Values = {
      0,          0x80000000, 1,          0x80000001, 0x007fffff, 0x807fffff,
      0x00800000, 0x3f800000, 0x40400000, 0xbf800000, 0x7e7fe800, 0x7e7fe801,
      0x7e800000, 0x7e800c00, 0x7e800c01, 0x7f7fffff, 0x7f800000, 0xff800000,
      0x7f800031, 0xff800071, 0x7fc00031};
  for (uint32_t Exponent = 1; Exponent < 255; Exponent += 7)
    for (uint32_t Fraction : {0U, 1U, 0x003fffffU, 0x007fffffU}) {
      Values.push_back((Exponent << 23) | Fraction);
      Values.push_back(0x80000000U | (Exponent << 23) | Fraction);
    }
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (bool Rsqrt : {false, true})
    for (unsigned Bytes : {4U, 16U, 32U})
      for (unsigned RC = 0; RC < 4; ++RC)
        for (unsigned Environment = 0; Environment < 4; ++Environment)
          for (unsigned Flags = 0; Flags < 3; ++Flags)
            for (unsigned Start = 0; Start < Values.size(); ++Start) {
              const uint32_t State = (Flags == 2 ? 0U : 0x1f80U) | (RC << 13) |
                                     ((Environment & 1) ? 0x40 : 0) |
                                     ((Environment & 2) ? 0x8000 : 0) |
                                     (Flags == 1 ? 0x3f : 0);
              std::vector<uint8_t> Source(Bytes);
              for (unsigned Lane = 0; Lane < Bytes / 4; ++Lane)
                std::memcpy(Source.data() + Lane * 4,
                            &Values[(Start + Lane * 3) % Values.size()], 4);
              NdOpEmulator Emu(Image);
              Emu.setX86Approx12ReferenceMode(true);
              Emu.setMXCSR(State);
              Emu.setRegisterBytes(x86reg::XMM0, Source);
              ASSERT_TRUE(Emu.step(
                  approxOp(unsigned(Rsqrt) | (Bytes == 4 ? 2 : 0), Bytes)));
              ASSERT_TRUE(Emu.getRegisterBytes(0));
              const auto Result = *Emu.getRegisterBytes(0);
              EXPECT_EQ(Emu.getMXCSR(), State);
              for (unsigned Lane = 0; Lane < Bytes / 4; ++Lane) {
                uint32_t Input, Output;
                std::memcpy(&Input, Source.data() + Lane * 4, 4);
                std::memcpy(&Output, Result.data() + Lane * 4, 4);
                SCOPED_TRACE(testing::Message()
                             << "input=" << Input << " rsqrt=" << Rsqrt
                             << " state=" << State);
                checkApproxEnvelope(Input, Output, Rsqrt);
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
                uint32_t NativeState = State;
                checkApproxEnvelope(
                    Input, nativeApprox(Input, Rsqrt, NativeState), Rsqrt);
                EXPECT_EQ(NativeState, State);
#endif
              }
            }
}

TEST(X86FPApprox12Contract, ExactModeNeverPublishesAnUnprovedRepresentative) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Bytes : {4U, 16U, 32U})
    for (bool Strict : {false, true})
      for (bool Reference : {false, true}) {
        NdOpEmulator Emu(Image);
        Emu.setStrictMode(Strict);
        Emu.setX86Approx12ReferenceMode(Reference);
        std::vector<uint8_t> Source(Bytes, 0), Sentinel(Bytes, 0x59);
        const uint32_t Three = 0x40400000;
        std::memcpy(Source.data() + Bytes - 4, &Three, 4);
        Emu.setRegisterBytes(x86reg::XMM0, Source);
        Emu.setRegisterBytes(0, Sentinel);
        const bool Complete = Reference && !Strict;
        EXPECT_EQ(Emu.step(approxOp(Bytes == 4 ? 2 : 0, Bytes)), Complete);
        EXPECT_EQ(Emu.skips().ApproximatedOps, Complete ? 1U : 0U);
        if (!Complete)
          EXPECT_EQ(Emu.getRegisterBytes(0), Sentinel);
        if (Complete) {
          Emu.reset();
          Emu.setRegisterBytes(x86reg::XMM0, Source);
          EXPECT_TRUE(Emu.step(approxOp(Bytes == 4 ? 2 : 0, Bytes)));
          EXPECT_EQ(Emu.skips().ApproximatedOps, 2U);
        }
      }
}

TEST(X86FPApprox12Contract, UnknownCSRRemainsUnknownAcrossBothSourceRoutes) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  addApproxMemory(Image, 0x2000, 16);
  for (bool Memory : {false, true})
    for (bool Normal : {false, true}) {
      NdOpEmulator Emu(Image);
      Emu.setCallPreservedRegisters({});
      Emu.setX86LinearAddressBits(48);
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(0x3000, 8));
      ASSERT_TRUE(Emu.step(Call));
      const auto Op = approxOp(2, 4, Memory);
      if (Memory) {
        Image.Segments[0].Data[2] = Normal ? 0x40 : 0;
        Image.Segments[0].Data[3] = Normal ? 0x40 : 0;
      } else
        Emu.setRegister(x86reg::XMM0, Normal ? 0x40400000 : 0);
      Emu.setX86Approx12ReferenceMode(Normal);
      ASSERT_TRUE(Emu.step(Op));
      EXPECT_EQ(Emu.skips().ApproximatedOps, Normal ? 1U : 0U);
      LowOp Read;
      Read.Opcode = NdOp::INTRINSIC;
      Read.Output = NdVar::tmp(99, 4);
      Read.addInput(NdVar::cst(unsigned(Intrinsic::X86ReadMXCSR), 2));
      EXPECT_FALSE(Emu.step(Read));
      EXPECT_FALSE(Emu.getRegisterBytes(99));
    }
}

TEST(X86FPApprox12Contract, MemoryAlignmentCanonicalRangeAndReadPermissions) {
  for (auto Space : {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
                     NdMemoryAddressSpace::X86GS})
    for (unsigned Bytes : {4U, 16U, 32U})
      for (bool VexPacked : {false, true})
        for (bool Unaligned : {false, true}) {
          if ((Bytes == 4 && VexPacked) || (Bytes == 32 && !VexPacked))
            continue;
          BinaryImage Image;
          Image.Arch = Arch::X64;
          addApproxMemory(Image, 0x2000, 64);
          const uint64_t Address = 0x2000 + unsigned(Unaligned);
          const auto Op = approxOp(
              (Bytes == 4 ? 2 : 0) | (VexPacked ? 4 : 0), Bytes, true,
              Space == NdMemoryAddressSpace::Default ? Address
                                                     : Address - 0x1000,
              Space);
          NdOpEmulator Emu(Image);
          Emu.setStrictMode(true);
          Emu.setMXCSR(0x3f);
          EXPECT_FALSE(Emu.step(Op)); // linear address context is unknown.
          ASSERT_TRUE(Emu.setX86LinearAddressBits(48));
          if (Space != NdMemoryAddressSpace::Default) {
            EXPECT_FALSE(Emu.step(Op));
            ASSERT_TRUE(Emu.setMemoryAddressSpaceBase(Space, 0x1000));
          }
          const bool Allowed = !Unaligned || Bytes == 4 || VexPacked;
          EXPECT_EQ(Emu.step(Op), Allowed);
          EXPECT_EQ(Emu.getMXCSR(), 0x3fU);
          Image.Segments[0].Flags = SegmentFlags::Writable;
          EXPECT_FALSE(Emu.step(Op));
        }
  for (unsigned Bits : {48U, 57U}) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    const uint64_t Last = (UINT64_C(1) << (Bits - 1)) - 1;
    addApproxMemory(Image, Last - 3, 16);
    NdOpEmulator Emu(Image);
    ASSERT_TRUE(Emu.setX86LinearAddressBits(Bits));
    EXPECT_TRUE(Emu.step(approxOp(2, 4, true, Last - 3)));
    EXPECT_FALSE(Emu.step(approxOp(2, 4, true, Last - 2)));
    EXPECT_FALSE(Emu.step(approxOp(2, 4, true, Last + 1)));
  }
}

TEST(X86FPApprox12Contract, MalformedShapesAndScalarProvenance) {
  for (bool Memory : {false, true})
    for (unsigned Bytes : {4U, 16U, 32U}) {
      auto Op = approxOp((Bytes == 4 ? 2 : 0) | (Memory && Bytes == 32 ? 4 : 0),
                         Bytes, Memory);
      const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
      const auto Shape = x86FPStateLowShape(Op, Arch::X64);
      ASSERT_TRUE(x86FPStateShapeIsValid(Id, Shape));
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 0, Bytes),
                Bytes == 4 ? 4U : 0U);
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 1, 3), 0U);
      for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
        auto Bad = Shape;
        switch (Mutation) {
        case 0:
          Bad.IdValue |= UINT64_C(1) << 16;
          break;
        case 1:
          Bad.Control |= 8;
          break;
        case 2:
          Bad.ControlIsConst = false;
          break;
        case 3:
          Bad.ControlSize = 4;
          break;
        case 4:
          Bad.OutputSize = 8;
          break;
        case 5:
          Bad.NumInputs = 4;
          break;
        case 6:
          Bad.HasAuxiliaryOutputs = true;
          break;
        case 7:
          Bad.TargetArch = Arch::ARM;
          break;
        case 8:
          Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
          break;
        }
        EXPECT_FALSE(x86FPStateShapeIsValid(Id, Bad));
      }
    }
}

TEST(X86FPApprox12Contract, RawBytesAndDescriptorsAgreeInBothModes) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (unsigned Prefix : {0U, 1U, 2U})
        for (bool Scalar : {false, true})
          for (bool Memory : {false, true})
            for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
              Decoder Decode;
              ASSERT_TRUE(Decode.init(Target));
              Decode.setStrict(Strict);
              std::vector<uint8_t> Bytes;
              if (Prefix == 1)
                Bytes = {0xc5, uint8_t(Scalar ? 0xea : 0xf8)};
              else if (Prefix == 2)
                Bytes = {0xc4, 0xe1, uint8_t(Scalar ? 0x6a : 0x78)};
              else {
                if (Scalar)
                  Bytes.push_back(0xf3);
                Bytes.push_back(0x0f);
              }
              Bytes.insert(Bytes.end(), {0x53, uint8_t(Memory ? 0x00 : 0xc1)});
              DecodedInsn Insn;
              ASSERT_EQ(
                  Decode.decodeOne(Bytes.data(), Bytes.size(), Entry, Insn),
                  Bytes.size());
              auto &Detail = Insn.Raw->detail->x86;
              const unsigned Source = Prefix && Scalar ? 2 : 1;
              if (Mutation == 0) {
                std::vector<LowOp> Ops;
                ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops));
                unsigned Found = 0;
                for (const auto &Op : Ops)
                  if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                      isX86FPApprox12Intrinsic(
                          static_cast<Intrinsic>(Op.Inputs[0].Offset))) {
                    ++Found;
                    EXPECT_TRUE(x86FPStateShapeIsValid(
                        static_cast<Intrinsic>(Op.Inputs[0].Offset),
                        x86FPStateLowShape(Op, Target)));
                  }
                EXPECT_EQ(Found, 1U);
                continue;
              }
              if (Mutation == 1)
                Detail.operands[0].size = 32;
              else if (Mutation == 2)
                Detail.operands[0].reg = X86_REG_XMM3;
              else if (Mutation == 3)
                Detail.operands[Source].size = 8;
              else if (Mutation == 4)
                Insn.Raw->bytes[0] = 0x66;
              else if (Mutation == 5)
                Insn.Raw->size = 16;
              else
                Detail.op_count++;
              std::vector<LowOp> Ops;
              EXPECT_THROW(Decode.liftToLow(Insn, Ops), UnliftedInstruction);
            }
}

TEST(X86FPApprox12Contract, HighAndMedRejectExtraRolesAndWrongConstants) {
  for (bool Memory : {false, true}) {
    const auto Id = Memory ? Intrinsic::X86FPApprox12MemoryState
                           : Intrinsic::X86FPApprox12State;
    MedOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = {
        .Kind = MedVar::Temp, .TheArch = Arch::X64, .Id = 1, .Size = 4};
    Op.addInput(MedVar::makeConst(unsigned(Id), 2));
    Op.addInput(MedVar::makeConst(Memory ? 0x2000 : 2, Memory ? 8 : 1));
    Op.addInput(MedVar::makeConst(Memory ? 2 : 0, Memory ? 1 : 4));
    ASSERT_TRUE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Op)));
    auto Call = HighExpr::makeCall(
        "", 0,
        {HighExpr::makeConst(Memory ? 0x2000 : 2, Memory ? 8 : 1),
         HighExpr::makeConst(Memory ? 2 : 0, Memory ? 1 : 4)});
    Call->IntrinsicId = Id;
    Call->Type = NdType::makeInt(4, false);
    ASSERT_TRUE(
        x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
    Call->Operands[Memory ? 1 : 0]->Type = NdType::makeFloat(1);
    EXPECT_FALSE(
        x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
    Call->Operands[Memory ? 1 : 0]->Type = NdType::makeInt(1, false);
    Call->IntrinsicOutputs.push_back(
        MedVar{.Kind = MedVar::Temp, .Id = 2, .Size = 4});
    EXPECT_FALSE(
        x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
    Op.IntrinsicOutputs = Call->IntrinsicOutputs;
    EXPECT_FALSE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Op)));
  }
}

TEST(X86FPApprox12Contract,
     OwnedAssemblyAuthenticatesTypesAddressSpaceAndEffects) {
  for (bool Memory : {false, true})
    for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
      llvm::LLVMContext Context;
      llvm::Module Module("approx12-owned", Context);
      llvm::IRBuilder<> Builder(Context);
      const unsigned Layout = x86FPRoundStateLayout(
          4, 2, 0,
          Memory ? NdMemoryAddressSpace::X86FS : NdMemoryAddressSpace::Default);
      auto *Type = Builder.getFloatTy();
      llvm::Type *Input =
          Memory ? static_cast<llvm::Type *>(
                       Builder.getPtrTy(Mutation == 1 ? 0 : 257))
                 : static_cast<llvm::Type *>(
                       Mutation == 1 ? Builder.getDoubleTy() : Type);
      auto *Fn = llvm::Function::Create(
          llvm::FunctionType::get(Type, {Input}, false),
          llvm::Function::ExternalLinkage, "approx12", Module);
      Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Fn));
      auto *Asm = llvm::InlineAsm::get(
          llvm::FunctionType::get(Type, {Input}, false),
          x86FPApprox12Asm(Layout, Memory),
          Mutation == 2 ? (Memory ? "=x,r" : "=x,x")
                        : (Memory ? X86FPApprox12MemoryConstraints
                                  : X86FPApprox12ValueConstraints),
          Mutation != 3);
      auto *Call = Builder.CreateCall(Asm, {Fn->getArg(0)});
      Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(Context, {}));
      Builder.CreateRet(Call);
      if (!Mutation)
        ASSERT_EQ(classifyX86FPStateAsm(*Call),
                  std::make_optional(
                      std::pair{Memory ? Intrinsic::X86FPApprox12MemoryState
                                       : Intrinsic::X86FPApprox12State,
                                Layout}));
      else
        EXPECT_DEATH(classifyX86FPStateAsm(*Call),
                     "invalid x86 FP state assembly");
    }
}

class X86FPApprox12Accuracy : public X86FPStateFixture {
protected:
  unsigned SourceBytes = 4;
  bool Unaligned = false;
  bool AlignmentFault = false;
  bool KeepResult = true;
  NdMemoryAddressSpace SourceSegment = NdMemoryAddressSpace::Default;

  std::string comparisonDriver(bool, bool Declare, bool Fault,
                               bool NativeCall = false) override {
    if (Fault) {
      std::string Text = R"(
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif
static _Alignas(64) unsigned char actual[80];
extern void native_probe(uintptr_t);
static int unchanged(void) {
  for(unsigned i=48;i<80;++i) if(actual[i]!=(unsigned char)(0x59u+i*37u)) return 0;
  return 1;
}
#if defined(_WIN32)
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *info) {
  if(info->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->ExceptionCode==EXCEPTION_DATATYPE_MISALIGNMENT)
    ExitProcess(unchanged() ? 0 : 1);
  return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void on_fault(int number) { _exit(number==SIGSEGV && unchanged() ? 0 : 1); }
#endif
)";
      if (Declare)
        Text += "extern void isa_probe(uintptr_t);\n";
      Text += R"(
int main(void) {
#if defined(_WIN32)
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  if(!AddVectoredExceptionHandler(1,on_fault)) return 77;
#else
  if(signal(SIGSEGV,on_fault)==SIG_ERR) return 77;
#endif
  for(unsigned i=0;i<80;++i) actual[i]=(unsigned char)(0x59u+i*37u);
  _mm_setcsr(0x1f80);
)";
      Text += NativeCall ? "native_probe((uintptr_t)actual);\n"
                         : "isa_probe((uintptr_t)actual);\n";
      return Text + "return 99; }\n";
    }
    auto Text = floatingDriver(false, Declare);
    const auto Replace = [&](const std::string &Old, const std::string &New) {
      const auto Position = Text.find(Old);
      if (Position != std::string::npos)
        Text.replace(Position, Old.size(), New);
    };
    Replace("0x33800000, 0x40000000",
            "0x33800000, 0x40000000, 0x80000001, 0x807fffff, 0x7e7fe800, "
            "0x7e800000, 0x7e800c01, 0x40400000, 0xff800071");
    Replace("unsigned char expected[80], actual[80];",
            "_Alignas(64) unsigned char expected[80], actual[80];");
    Replace("memcpy(expected + 16, &values[b], sizeof(values[0]));",
            "for (unsigned lane = 0; lane < " +
                std::to_string(SourceBytes / 4) +
                "; ++lane) memcpy(expected + " + (Unaligned ? "17" : "16") +
                " + lane * 4, &values[(b + lane * 3) % "
                "(sizeof(values)/sizeof(values[0]))], 4);");
    Replace("for (unsigned sticky = 0; sticky < 2; ++sticky)",
            "for (unsigned sticky = 0; sticky < 3; ++sticky)");
    Replace("(sticky ? 0x25 : 0);",
            "(sticky == 1 ? 0x3f : 0);\n"
            "            if (sticky == 2) state &= ~0x1f80u;");
#if defined(__linux__)
    if (SourceSegment == NdMemoryAddressSpace::X86GS) {
      Replace("#include <stdint.h>",
              "#include <stdint.h>\n#include <unistd.h>\n#include "
              "<sys/syscall.h>\n#include <asm/prctl.h>");
      Replace(
          "unsigned count = 0;",
          "unsigned count = 0; unsigned long saved_gs;\n"
          "  if (syscall(SYS_arch_prctl, ARCH_GET_GS, &saved_gs)) return 77;");
      Replace("native_probe((uintptr_t)expected);",
              "if (syscall(SYS_arch_prctl, ARCH_SET_GS, expected)) return 77;\n"
              "            native_probe((uintptr_t)expected);");
      Replace("isa_probe((uintptr_t)actual);",
              "if (syscall(SYS_arch_prctl, ARCH_SET_GS, actual)) return 77;\n"
              "            isa_probe((uintptr_t)actual);");
      Replace("uint32_t actual_state = _mm_getcsr();",
              "uint32_t actual_state = _mm_getcsr();\n"
              "            if (syscall(SYS_arch_prctl, ARCH_SET_GS, saved_gs)) "
              "return 77;");
    }
#endif
    return Text;
  }

  void compareApproxReturn(bool Rsqrt) {
    const bool IsDouble = false;
    if (!nativeX64())
      GTEST_SKIP() << "native x86_64 host required";
    const std::vector<uint8_t> Bytes = {
        0xf3, 0x0f, static_cast<uint8_t>(Rsqrt ? 0x52 : 0x53), 0xc0, 0xc3};
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
    const auto Common = floatingDriver(IsDouble, false);
    const auto Values = Common.find("static const");
    const auto Main = Common.find("int main(void)");
    ASSERT_NE(Values, std::string::npos);
    ASSERT_NE(Main, std::string::npos);
    std::string Driver = "\n#include <stdint.h>\n#include <string.h>\n"
                         "#include <immintrin.h>\nextern " +
                         Scalar + " native_probe(" + Scalar + ");\n" +
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
    Driver += Scalar +
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

  void check(bool Rsqrt, bool Scalar, bool Vex, unsigned Width,
             bool Memory = false) {
    const bool AVX = llvm::sys::getHostCPUFeatures().lookup("avx");
    if (Vex && !AVX)
      GTEST_SKIP() << "native AVX and OS vector state required";
    SourceBytes = Scalar ? 4 : Width;
    const auto Arg = memoryModRM();
    std::vector<uint8_t> Bytes;
    if (AVX)
      Bytes = {0xc5, 0xfe, 0x6f, Arg};
    else
      Bytes = {0xf3, 0x0f, 0x6f, Arg};
    if (!Memory) {
      if (Width == 32)
        Bytes.insert(Bytes.end(),
                     {0xc5, 0xfe, 0x6f, static_cast<uint8_t>(0x48 | Arg), 16});
      else
        Bytes.insert(Bytes.end(),
                     {0xf3, 0x0f, 0x6f, static_cast<uint8_t>(0x48 | Arg), 16});
    }
    if (Vex && Scalar)
      Bytes.insert(Bytes.end(),
                   {0xf3, 0x0f, 0x6f, static_cast<uint8_t>(0x10 | Arg)});
    if (SourceSegment != NdMemoryAddressSpace::Default)
      Bytes.push_back(SourceSegment == NdMemoryAddressSpace::X86FS ? 0x64
                                                                   : 0x65);
    if (Vex)
      Bytes.insert(Bytes.end(), {0xc5, uint8_t(Scalar        ? 0xea
                                               : Width == 32 ? 0xfc
                                                             : 0xf8)});
    else {
      if (Scalar)
        Bytes.push_back(0xf3);
      Bytes.push_back(0x0f);
    }
    Bytes.push_back(Rsqrt ? 0x52 : 0x53);
    if (Memory && SourceSegment != NdMemoryAddressSpace::Default) {
#if defined(_WIN32)
      const uint8_t Offset = 0x30;
#else
      const uint8_t Offset =
          SourceSegment == NdMemoryAddressSpace::X86FS ? 0 : 16;
#endif
      Bytes.insert(Bytes.end(), {0x04, 0x25, Offset, 0, 0, 0});
    } else if (Memory)
      Bytes.insert(Bytes.end(), {static_cast<uint8_t>(0x40 | Arg),
                                 uint8_t(Unaligned ? 17 : 16)});
    else
      Bytes.push_back(0xc1);
    if (KeepResult && AVX)
      Bytes.insert(Bytes.end(),
                   {0xc5, 0xfe, 0x7f, static_cast<uint8_t>(0x40 | Arg), 48});
    else if (KeepResult)
      Bytes.insert(Bytes.end(),
                   {0xf3, 0x0f, 0x7f, static_cast<uint8_t>(0x40 | Arg), 48});
    Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
    compareFloating(Bytes, false, AlignmentFault);
  }
};

#define APPROX_TEST(Name, Rsqrt, Scalar, Vex, Width, Memory)                   \
  TEST_F(X86FPApprox12Accuracy, Name) {                                        \
    check(Rsqrt, Scalar, Vex, Width, Memory);                                  \
  }
APPROX_TEST(RcpScalarRegister, false, true, false, 16, false)
APPROX_TEST(RsqrtScalarRegister, true, true, false, 16, false)
APPROX_TEST(RcpPackedRegister, false, false, false, 16, false)
APPROX_TEST(RsqrtPackedRegister, true, false, false, 16, false)
APPROX_TEST(RcpScalarMemory, false, true, false, 16, true)
APPROX_TEST(RsqrtScalarMemory, true, true, false, 16, true)
APPROX_TEST(RcpPackedMemory, false, false, false, 16, true)
APPROX_TEST(RsqrtPackedMemory, true, false, false, 16, true)
APPROX_TEST(VexRcpScalarRegister, false, true, true, 16, false)
APPROX_TEST(VexRsqrtScalarRegister, true, true, true, 16, false)
APPROX_TEST(VexRcpPacked128Register, false, false, true, 16, false)
APPROX_TEST(VexRsqrtPacked128Register, true, false, true, 16, false)
APPROX_TEST(VexRcpPacked256Register, false, false, true, 32, false)
APPROX_TEST(VexRsqrtPacked256Register, true, false, true, 32, false)
APPROX_TEST(VexRcpScalarMemory, false, true, true, 16, true)
APPROX_TEST(VexRsqrtScalarMemory, true, true, true, 16, true)
APPROX_TEST(VexRcpPacked128Memory, false, false, true, 16, true)
APPROX_TEST(VexRsqrtPacked128Memory, true, false, true, 16, true)
APPROX_TEST(VexRcpPacked256Memory, false, false, true, 32, true)
APPROX_TEST(VexRsqrtPacked256Memory, true, false, true, 32, true)
#undef APPROX_TEST
TEST_F(X86FPApprox12Accuracy, ScalarMemoryAllowsUnalignedSource) {
  Unaligned = true;
  check(false, true, false, 16, true);
}
TEST_F(X86FPApprox12Accuracy, VexPackedMemoryAllowsUnalignedSource) {
  Unaligned = true;
  check(true, false, true, 32, true);
}
TEST_F(X86FPApprox12Accuracy, LegacyPackedMemoryFaultsBeforeDestinationWrite) {
  Unaligned = AlignmentFault = true;
  check(false, false, false, 16, true);
}
TEST_F(X86FPApprox12Accuracy, DiscardedResultRetainsTheMemoryFault) {
  KeepResult = false;
  Unaligned = AlignmentFault = true;
  check(true, false, false, 16, true);
}
TEST_F(X86FPApprox12Accuracy, GSScalarMemoryRetainsItsAddressSpace) {
  SourceSegment = NdMemoryAddressSpace::X86GS;
  check(false, true, false, 16, true);
}
TEST_F(X86FPApprox12Accuracy, FSTLSScalarMemoryRetainsItsAddressSpace) {
#if defined(__linux__)
  SourceSegment = NdMemoryAddressSpace::X86FS;
  check(true, true, true, 16, true);
#else
  GTEST_SKIP() << "native Linux FS TLS source required";
#endif
}
TEST_F(X86FPApprox12Accuracy, ScalarReturnPreservesBitsAndFPType) {
  compareApproxReturn(false);
  compareApproxReturn(true);
}
} // namespace
