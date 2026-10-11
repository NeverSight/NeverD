//===- X86_64_FPPackedConvertStateTests.cpp - Packed completion checks
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original SSE/VEX packed conversion bytes versus complete numerical/state
/// output.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/ir/high/X86FPStateShape.h"

#include <array>
#include <type_traits>

namespace {
struct PackedForm {
  unsigned Kind;
  bool Vex;
  unsigned Width;
  bool Memory;
};
constexpr std::array<uint8_t, 8> Opcodes{0x5b, 0xe6, 0x5a, 0x5a,
                                         0x5b, 0xe6, 0x5b, 0xe6};
constexpr std::array<uint8_t, 8> Prefixes{0, 2, 0, 1, 1, 3, 2, 1};
constexpr std::array<const char *, 8> Names{"DqToPs",   "DqToPd",  "PsToPd",
                                            "PdToPs",   "PsToDq",  "PdToDq",
                                            "TtPsToDq", "TtPdToDq"};
std::vector<PackedForm> packedForms() {
  std::vector<PackedForm> Result;
  for (unsigned Kind = 0; Kind < 8; ++Kind)
    for (bool Memory : {false, true})
      for (auto [Vex, Width] :
           {std::pair{false, 16U}, std::pair{true, 16U}, std::pair{true, 32U}})
        Result.push_back({Kind, Vex, Width, Memory});
  return Result;
}
std::string formName(const PackedForm &Form) {
  return std::string(Names[Form.Kind]) + (Form.Vex ? "Vex" : "Legacy") +
         std::to_string(Form.Width * 8) + (Form.Memory ? "Memory" : "Register");
}

LowOp packedStateOp(unsigned Control, uint32_t State, bool Memory = false) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(2, x86FPPackedConvertDestinationBytes(Control) + 4);
  Op.addInput(
      NdVar::cst(unsigned(Memory ? Intrinsic::X86FPPackedConvertMemoryState
                                 : Intrinsic::X86FPPackedConvertState),
                 2));
  Op.addInput(
      NdVar::tmp(1, Memory ? 8 : x86FPPackedConvertSourceBytes(Control)));
  Op.addInput(NdVar::cst(Control, 1));
  Op.addInput(NdVar::cst(State, 4));
  return Op;
}
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
template <unsigned Width>
__attribute__((target("avx"))) std::pair<std::vector<uint8_t>, uint32_t>
nativePacked(unsigned Kind, bool Vex, llvm::ArrayRef<uint8_t> Input,
             uint32_t State) {
  const unsigned Saved = _mm_getcsr();
  std::vector<uint8_t> Result;
#define CONVERT_CASE(K, OP, SRC, DST)                                          \
  case K: {                                                                    \
    using SourceType = std::conditional_t<SRC == 32, __m256i, __m128i>;        \
    using ResultType = std::conditional_t<DST == 32, __m256i, __m128i>;        \
    SourceType Source{};                                                       \
    ResultType Converted;                                                      \
    std::memcpy(&Source, Input.data(), Input.size());                          \
    if (Vex)                                                                   \
      __asm__ volatile("ldmxcsr %1\n\tv" OP " %2,%0\n\tstmxcsr %1"             \
                       : "=&x"(Converted), "+m"(State)                         \
                       : "x"(Source)                                           \
                       : "memory");                                            \
    else if constexpr (Width == 16)                                            \
      __asm__ volatile("ldmxcsr %1\n\t" OP " %2,%0\n\tstmxcsr %1"              \
                       : "=&x"(Converted), "+m"(State)                         \
                       : "x"(Source)                                           \
                       : "memory");                                            \
    Result.resize(DST);                                                        \
    std::memcpy(Result.data(), &Converted, DST);                               \
    break;                                                                     \
  }
  switch (Kind) {
    CONVERT_CASE(0, "cvtdq2ps", Width, Width);
    CONVERT_CASE(1, "cvtdq2pd", 16, Width);
    CONVERT_CASE(2, "cvtps2pd", 16, Width);
    CONVERT_CASE(3, "cvtpd2ps", Width, 16);
    CONVERT_CASE(4, "cvtps2dq", Width, Width);
    CONVERT_CASE(5, "cvtpd2dq", Width, 16);
    CONVERT_CASE(6, "cvttps2dq", Width, Width);
    CONVERT_CASE(7, "cvttpd2dq", Width, 16);
  }
#undef CONVERT_CASE
  _mm_setcsr(Saved);
  return {Result, State};
}
#endif
TEST(PackedConvertContract, ConcreteResultPaddingAndMxcsrMatchNative) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "Native AVX required";
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  uint64_t Random = UINT64_C(0x456caf293ab478e1);
  const uint64_t Directed[] = {0,
                               1,
                               UINT64_MAX,
                               UINT64_C(0x1000001),
                               UINT64_C(0x80000000),
                               UINT64_C(0x3fc00000),
                               UINT64_C(0xbfc00000),
                               UINT64_C(0x7f800071),
                               UINT64_C(0x4f000000),
                               UINT64_C(0x7ff0000000000071),
                               UINT64_C(0x3ff8000000000000),
                               UINT64_C(0xbff8000000000000),
                               UINT64_C(0x7fefffffffffffff),
                               UINT64_C(0x36a0000000000000),
                               UINT64_C(0x380fffffc0000000),
                               UINT64_C(0x380fffffffffffff),
                               UINT64_C(0x41dfffffffc00000),
                               UINT64_C(0xc1e0000000200000)};
  for (unsigned Control = 0; Control < 32; ++Control) {
    if (!x86FPPackedConvertControlIsValid(Control))
      continue;
    const unsigned SourceBytes = x86FPPackedConvertSourceBytes(Control);
    const unsigned Element = x86FPPackedConvertSourceElementBytes(Control);
    for (unsigned Environment = 0; Environment < 32; ++Environment)
      for (unsigned I = 0; I < 256; ++I) {
        std::vector<uint8_t> Input(SourceBytes);
        for (unsigned Lane = 0; Lane < SourceBytes / Element; ++Lane) {
          Random ^= Random << 13;
          Random ^= Random >> 7;
          Random ^= Random << 17;
          uint64_t Bits = I < std::size(Directed)
                              ? Directed[(I + Lane * 3) % std::size(Directed)]
                              : Random;
          std::memcpy(Input.data() + Lane * Element, &Bits, Element);
        }
        const uint32_t State = 0x1f80 | ((Environment & 3) << 13) |
                               ((Environment & 4) ? 0x40 : 0) |
                               ((Environment & 8) ? 0x8000 : 0) |
                               ((Environment & 16) ? 0x25 : 0);
        const auto Expected =
            (Control & 16) ? nativePacked<32>(Control & 7, true, Input, State)
                           : nativePacked<16>(Control & 7, (Control & 8) != 0,
                                              Input, State);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegisterBytes(1, Input);
        const auto Op = packedStateOp(Control, State);
        SCOPED_TRACE(testing::Message()
                     << Control << ':' << Environment << ':' << I);
        ASSERT_TRUE(E.step(Op));
        ASSERT_TRUE(E.getRegisterBytes(2));
        const auto Result = *E.getRegisterBytes(2);
        ASSERT_EQ(Result.size(), Expected.first.size() + 4);
        ASSERT_TRUE(std::equal(Expected.first.begin(), Expected.first.end(),
                               Result.begin()));
        uint32_t Outgoing = 0;
        std::memcpy(&Outgoing, Result.data() + Expected.first.size(), 4);
        ASSERT_EQ(Outgoing, Expected.second);
        ASSERT_EQ(E.getMXCSR(), Expected.second);
      }
  }
#else
  GTEST_SKIP() << "Native x64 instruction oracle requires GCC/Clang";
#endif
}

TEST(PackedConvertContract, ShapeAndTypeRolesPreservePackedCarrierIdentity) {
  for (unsigned Control = 0; Control < 32; ++Control) {
    if (!x86FPPackedConvertControlIsValid(Control))
      continue;
    for (bool Memory : {false, true}) {
      auto Op = packedStateOp(Control, 0x1f80, Memory);
      const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
      const auto Shape = x86FPStateLowShape(Op, Arch::X64);
      ASSERT_TRUE(x86FPStateShapeIsValid(Id, Shape));
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 0, 4), 0U);
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 0, 8), 0U);
      EXPECT_EQ(
          x86FPStateDestinationBytes(Id, x86FPStateHelperLayout(Id, Shape)),
          Op.Output.Size - 4);
      for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
        auto Bad = Op;
        switch (Mutation) {
        case 0:
          Bad.Inputs[2] = NdVar::tmp(9, 1);
          break;
        case 1:
          Bad.Inputs[2].Offset |= 32;
          break;
        case 2:
          Bad.Inputs[2].Size = 2;
          break;
        case 3:
          Bad.Inputs[3].Size = 8;
          break;
        case 4:
          Bad.Inputs[1].Size = Memory ? 4 : 64;
          break;
        case 5:
          Bad.Output.Size -= 4;
          break;
        case 6:
          Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
          break;
        case 7:
          Bad.Inputs[0].Size = 4;
          break;
        }
        EXPECT_FALSE(
            x86FPStateShapeIsValid(Id, x86FPStateLowShape(Bad, Arch::X64)));
      }
      auto Call = HighExpr::makeCall("", 0,
                                     {HighExpr::makeConst(0, Op.Inputs[1].Size),
                                      HighExpr::makeConst(Control, 1),
                                      HighExpr::makeConst(0x1f80, 4)});
      Call->IntrinsicId = Id;
      Call->Type = NdType::makeInt(Op.Output.Size, false);
      ASSERT_TRUE(
          x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
      Call->Operands[0]->Type = NdType::makeFloat(Op.Inputs[1].Size);
      EXPECT_FALSE(
          x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
      Call->Operands[0]->Type = NdType::makeInt(Op.Inputs[1].Size, false);
      Call->IsIndirectCall = true;
      EXPECT_FALSE(
          x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
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
      Med.Inputs[3].TheArch = Arch::Unknown;
      Med.IntrinsicOutputs.push_back(
          {.Kind = MedVar::Temp, .Id = 4, .Size = 4});
      EXPECT_FALSE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Med)));
    }
  }
}

TEST(PackedConvertContract,
     OwnedAssemblyRequiresPhysicalCarrierSourceAndStateSpaces) {
  for (unsigned Control : {1U, 3U, 8U, 10U, 25U, 27U, 29U, 31U})
    for (bool Memory : {false, true})
      for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
        SCOPED_TRACE(testing::Message()
                     << Control << ':' << Memory << ':' << Mutation);
        llvm::LLVMContext C;
        llvm::Module Module("packed-owned", C);
        llvm::IRBuilder<> B(C);
        const auto Space = Memory ? NdMemoryAddressSpace::X86GS
                                  : NdMemoryAddressSpace::Default;
        const unsigned Layout = x86FPRoundStateLayout(
            x86FPPackedConvertSourceBytes(Control), Control, 0, Space);
        llvm::Type *Result = x86FPPackedConvertResultType(C, Control);
        llvm::Type *Source = Memory ? static_cast<llvm::Type *>(B.getPtrTy(256))
                                    : x86FPPackedConvertSourceType(C, Control);
        llvm::Type *State = B.getPtrTy(Mutation == 2 ? 257 : 0);
        if (Mutation == 1)
          Source =
              Memory ? static_cast<llvm::Type *>(B.getPtrTy()) : B.getInt64Ty();
        if (Mutation == 3)
          Result = B.getInt64Ty();
        auto *Fn = llvm::Function::Create(
            llvm::FunctionType::get(Result, {Source, State}, false),
            llvm::Function::ExternalLinkage, "packed", Module);
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
        auto Text = x86FPPackedConvertAsm(Layout, Memory);
        auto Constraints = std::string(x86FPPackedConvertConstraints(Memory));
        if (Mutation == 4)
          Text += "\n\tnop";
        if (Mutation == 5)
          Constraints.resize(Constraints.find(",~{memory}"));
        auto *Asm =
            llvm::InlineAsm::get(Fn->getFunctionType(), Text, Constraints,
                                 Mutation != 6, Mutation == 7,
                                 Mutation == 8 ? llvm::InlineAsm::AD_Intel
                                               : llvm::InlineAsm::AD_ATT);
        auto *Call = B.CreateCall(Asm, {Fn->getArg(0), Fn->getArg(1)});
        if (Mutation != 9)
          Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(C, {}));
        B.CreateRet(Call);
        if (!Mutation)
          EXPECT_EQ(classifyX86FPStateAsm(*Call),
                    std::make_optional(std::pair{
                        Memory ? Intrinsic::X86FPPackedConvertMemoryState
                               : Intrinsic::X86FPPackedConvertState,
                        Layout}));
        else if (Mutation == 9)
          EXPECT_FALSE(classifyX86FPStateAsm(*Call));
        else
          EXPECT_DEATH(classifyX86FPStateAsm(*Call),
                       "invalid x86 FP state assembly");
      }
}

TEST(PackedConvertContract,
     CheckedMemoryOwnsSourceExtentAlignmentAndFaultState) {
  for (unsigned Control = 0; Control < 32; ++Control) {
    if (!x86FPPackedConvertControlIsValid(Control))
      continue;
    const unsigned Bytes = x86FPPackedConvertSourceBytes(Control);
    for (auto Space :
         {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
          NdMemoryAddressSpace::X86GS})
      for (unsigned Fault = 0; Fault < 5; ++Fault) {
        SCOPED_TRACE(testing::Message()
                     << Control << ':' << unsigned(Space) << ':' << Fault);
        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        Segment S;
        S.VA = 0x2010;
        S.Size = S.FileSz = Bytes + (Fault == 4 ? 1 : 0) - (Fault == 2 ? 1 : 0);
        S.Data.resize(S.Size);
        S.Flags = Fault == 1 ? SegmentFlags::Writable : SegmentFlags::Readable;
        Image.Segments.push_back(S);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setMXCSR(0x3f80);
        if (Fault != 3) {
          E.setX86LinearAddressBits(48);
          if (Space != NdMemoryAddressSpace::Default)
            E.setMemoryAddressSpaceBase(Space, 0x1000);
        }
        E.setRegister(
            1, (Space == NdMemoryAddressSpace::Default ? 0x2010 : 0x1010) +
                   (Fault == 4 ? 1 : 0));
        auto Op = packedStateOp(Control, 0x1f80, true);
        Op.MemoryAddressSpace = Space;
        const bool Complete =
            Fault == 0 ||
            (Fault == 4 && x86FPPackedConvertMemoryAlignment(Control) == 1);
        EXPECT_EQ(E.step(Op), Complete);
        EXPECT_EQ(bool(E.getRegisterBytes(2)), Complete);
        EXPECT_EQ(E.getMXCSR(), Fault == 3 ? 0x3f80U : 0x1f80U);
      }
  }
}

TEST(PackedConvertContract,
     UnmaskedInputExceptionsKeepPriorDestinationAndStickyFlags) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (unsigned Control : {4U, 12U, 28U})
    for (unsigned Sticky : {0U, 0x25U}) {
      const unsigned Bytes = x86FPPackedConvertSourceBytes(Control);
      std::vector<uint8_t> Input(Bytes);
      const uint32_t Values[] = {0x3fc00000, 0x7f800071};
      std::memcpy(Input.data(), Values, sizeof(Values));
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegisterBytes(1, Input);
      const auto Op = packedStateOp(Control, 0x1f00 | Sticky);
      const std::vector<uint8_t> Before(Op.Output.Size, 0xa7);
      E.setRegisterBytes(2, Before);
      EXPECT_FALSE(E.step(Op));
      EXPECT_EQ(E.getMXCSR(), 0x1f01U | Sticky);
      ASSERT_TRUE(E.getRegisterBytes(2));
      EXPECT_EQ(*E.getRegisterBytes(2), Before);
    }
}

TEST(PackedConvertContract, RawEncodingAndDecodedMetadataMustAgree) {
  for (const auto &Form : packedForms())
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(testing::Message() << formName(Form) << ':' << Mutation);
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Arch::X64));
      std::vector<uint8_t> Bytes;
      if (Form.Vex)
        Bytes = {0xc5, uint8_t(0xf8 | Prefixes[Form.Kind] |
                               (Form.Width == 32 ? 4 : 0))};
      else {
        const auto PP = Prefixes[Form.Kind];
        if (PP)
          Bytes.push_back(PP == 1 ? 0x66 : PP == 2 ? 0xf3 : 0xf2);
        Bytes.push_back(0x0f);
      }
      Bytes.insert(Bytes.end(),
                   {Opcodes[Form.Kind], uint8_t(Form.Memory ? 0x40 : 0xc1)});
      if (Form.Memory)
        Bytes.push_back(16);
      DecodedInsn Insn;
      ASSERT_EQ(
          Decode.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Insn),
          int(Bytes.size()));
      auto &X86 = Insn.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        break;
      case 1:
        X86.avx_sae = true;
        break;
      case 2:
        X86.avx_rm = X86_AVX_RM_RD;
        break;
      case 3:
        X86.operands[0].size = 8;
        break;
      case 4:
        X86.operands[1].size = 64;
        break;
      case 5:
        X86.encoding.imm_size = 1;
        break;
      case 6:
        X86.operands[0].reg = X86_REG_XMM3;
        break;
      case 7:
        X86.modrm ^= 1;
        break;
      case 8:
        if (Form.Memory)
          X86.operands[1].mem.disp++;
        else
          X86.operands[1].reg = X86_REG_XMM3;
        break;
      case 9:
        Insn.Raw->size = 16;
        break;
      case 10:
        X86.operands[1].avx_bcast = X86_AVX_BCAST_4;
        break;
      case 11:
        X86.operands[0].avx_zero_opmask = true;
        break;
      }
      std::vector<LowOp> Ops;
      if (Mutation)
        EXPECT_THROW(Decode.liftToLow(Insn, Ops), UnliftedInstruction);
      else {
        ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops));
        unsigned Found = 0;
        for (const auto &Op : Ops)
          if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
              isX86FPPackedConvertStateIntrinsic(
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
class PackedConvertFixture : public X86FPStateFixture {
protected:
  PackedForm Form{};
  bool KeepResult = true;
  unsigned SourceOffset = 64;
  unsigned FaultMode = 0;
  unsigned SourceRegister = 1;
  bool GSSource = false;
  unsigned sourceElementBytes() const {
    return Form.Kind == 3 || Form.Kind == 5 || Form.Kind == 7 ? 8 : 4;
  }
  unsigned sourceBytes() const {
    return Form.Kind == 1 || Form.Kind == 2 ? Form.Width / 2 : Form.Width;
  }
  std::string conversionFaultDriver(bool Declare, bool NativeCall) {
    const bool MemoryFault = FaultMode >= 5;
    const bool PageFault = FaultMode == 5;
    const unsigned State = FaultMode == 0                     ? 0x0f80
                           : FaultMode == 1                   ? 0x1f00
                           : FaultMode == 2                   ? 0x1e80
                           : FaultMode == 3                   ? 0x1780
                           : FaultMode == 4 || FaultMode == 6 ? 0x1f00
                                                              : 0x1f80;
    const unsigned Raised = FaultMode == 0   ? 0x20
                            : FaultMode == 1 ? 1
                            : FaultMode == 2 ? 2
                            : FaultMode == 3 ? 0x10
                            : FaultMode == 4 ? 1
                                             : 0;
    const uint64_t Bits =
        FaultMode == 0 ? UINT64_C(0x1000001)
        : FaultMode == 1
            ? (sourceElementBytes() == 4 ? UINT64_C(0x7f800071)
                                         : UINT64_C(0x7ff0000000000071))
        : FaultMode == 2 ? 1
        : FaultMode == 4 ? UINT64_C(0x3fc00000)
        : FaultMode == 6 ? UINT64_C(0x7ff0000000000071)
                         : UINT64_C(0x36a0000000000000);
    std::string Text =
        "#define _GNU_SOURCE\n#include <stdint.h>\n"
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <immintrin.h>\nextern void native_probe(uintptr_t);\n";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += "#define MEMORY_FAULT " + std::to_string(MemoryFault) +
            "\n#define PAGE_FAULT " + std::to_string(PageFault) +
            "\n#define SOURCE_OFFSET " + std::to_string(SourceOffset) +
            "\n#define SOURCE_BYTES " + std::to_string(sourceBytes()) +
            "\n#define EXPECTED_STATE " + std::to_string(State | Raised) +
            "u\n";
    Text += R"(
static unsigned char *actual;
static unsigned char before[160];
static void check(unsigned state, uintptr_t address) {
  if (state != EXPECTED_STATE) {
    fprintf(stderr, "exception MXCSR expected=%08x actual=%08x\n", EXPECTED_STATE, state); _Exit(2);
  }
  if (PAGE_FAULT && (address < (uintptr_t)actual + SOURCE_OFFSET ||
      address >= (uintptr_t)actual + SOURCE_OFFSET + SOURCE_BYTES)) _Exit(3);
  if (memcmp(actual, before, PAGE_FAULT ? 64 : 160)) _Exit(4);
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
  if (PAGE_FAULT) {
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
  if (PAGE_FAULT) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *base = mmap(0, 2*page, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED || mprotect(base + page, page, PROT_NONE)) return 11;
    actual = base + page - 64;
  }
#endif
  for (unsigned i = 0; i < (PAGE_FAULT ? 64u : 160u); ++i)
    actual[i] = before[i] = (unsigned char)(0x59u + i*37u);
)";
    if (!PageFault)
      Text += "  uint64_t bits = " + std::to_string(Bits) +
              "ULL;\n"
              "  memset(actual + SOURCE_OFFSET,0,SOURCE_BYTES);\n"
              "  memcpy(actual + SOURCE_OFFSET, &bits, " +
              std::to_string(sourceElementBytes()) +
              ");\n"
              "  memcpy(before, actual, 160);\n";
    if (FaultMode == 4)
      Text +=
          "  uint32_t nan = 0x7f800071; memcpy(actual+SOURCE_OFFSET+4,&nan,4); "
          "memcpy(before,actual,160);\n";
    Text += "  _mm_setcsr(" + std::to_string(State) + ");\n";
    Text += NativeCall ? "  native_probe((uintptr_t)actual);\n"
                       : "  isa_probe((uintptr_t)actual);\n";
    return Text + "  return 99; /* required exception did not occur */\n}\n";
  }

  std::vector<uint8_t> kernel() {
    const auto Arg = memoryModRM();
    std::vector<uint8_t> Bytes{0xc5, 0xfe, 0x6f, Arg};
    if (!Form.Memory)
      Bytes.insert(Bytes.end(), {0xc5, 0xfe, 0x6f,
                                 uint8_t(0x40 | (SourceRegister << 3) | Arg),
                                 uint8_t(SourceOffset)});
    if (GSSource)
      Bytes.push_back(0x65);
    if (Form.Vex)
      Bytes.insert(Bytes.end(), {0xc5, uint8_t(0xf8 | Prefixes[Form.Kind] |
                                               (Form.Width == 32 ? 4 : 0))});
    else {
      const auto PP = Prefixes[Form.Kind];
      if (PP)
        Bytes.push_back(PP == 1 ? 0x66 : PP == 2 ? 0xf3 : 0xf2);
      Bytes.push_back(0x0f);
    }
    Bytes.push_back(Opcodes[Form.Kind]);
    if (Form.Memory && GSSource) {
#if defined(_WIN32)
      const uint8_t Offset = 0x30;
#else
      const uint8_t Offset = 64;
#endif
      Bytes.insert(Bytes.end(), {0x04, 0x25, Offset, 0, 0, 0});
    } else if (Form.Memory)
      Bytes.insert(Bytes.end(), {uint8_t(0x40 | Arg), uint8_t(SourceOffset)});
    else
      Bytes.push_back(uint8_t(0xc0 | SourceRegister));
    if (KeepResult)
      Bytes.insert(Bytes.end(), {0xc5, 0xfe, 0x7f, uint8_t(0x40 | Arg), 96});
    Bytes.insert(Bytes.end(),
                 {0x0f, 0xae, uint8_t(0x98 | Arg), 128, 0, 0, 0, 0xc3});
    return Bytes;
  }
  std::string comparisonDriver(bool, bool Declare, bool Fault,
                               bool NativeCall = false) override {
    if (Fault)
      return conversionFaultDriver(Declare, NativeCall);
    std::string Text =
        "#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n"
        "#include <immintrin.h>\nextern void native_probe(uintptr_t);\n";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += "static const uint64_t values[] = {";
    if (Form.Kind < 2)
      Text += R"(
0,1,0xffffffff,0x7fffffff,0x80000000,0x1000001,0xfeffffff,
0x1000003,0xfefffffd,0x40000001,0xc0000001,0x7fffff80,0x80000080
)";
    else if (sourceElementBytes() == 8)
      Text += R"(
0,0x8000000000000000ULL,0x3ff0000000000000ULL,0xbff0000000000000ULL,
0x3ff8000000000000ULL,0xbff8000000000000ULL,0x4004000000000000ULL,
1,0x000fffffffffffffULL,0x0010000000000000ULL,0x36a0000000000000ULL,
0x380fffffc0000000ULL,0x380fffffffffffffULL,0x7fefffffffffffffULL,
0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000031ULL,
0x7ff0000000000071ULL,0x41dfffffffc00000ULL,0xc1e0000000200000ULL
)";
    else
      Text += R"(
0,0x80000000,0x3f800000,0xbf800000,0x3fc00000,0xbfc00000,0x40200000,
1,0x007fffff,0x00800000,0x7f7fffff,0x7f800000,0xff800000,
0x7fc00031,0xffc00077,0x7f800071,0x4effffff,0xcf000001
)";
    Text += R"(
};
int main(void) {
  unsigned saved = _mm_getcsr();
  for(unsigned rounding=0;rounding<4;++rounding)
    for(unsigned environment=0;environment<4;++environment)
      for(unsigned sticky=0;sticky<2;++sticky)
        for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i) {
          _Alignas(64) unsigned char expected[160],actual[160];
          for(unsigned j=0;j<160;++j) expected[j]=(unsigned char)(0x59u+j*37u);
)";
    Text += "          for(unsigned lane=0;lane<" +
            std::to_string(sourceBytes() / sourceElementBytes()) +
            ";++lane) memcpy(expected+" + std::to_string(SourceOffset) +
            "+lane*" + std::to_string(sourceElementBytes()) +
            ", &values[(i+lane*3)%(sizeof(values)/sizeof(values[0]))]," +
            std::to_string(sourceElementBytes()) + ");\n";
    Text += R"(
          memcpy(actual,expected,160);
          unsigned state=0x1f80|(rounding<<13)|((environment&1)?0x40:0)|
                         ((environment&2)?0x8000:0)|(sticky?0x25:0);
          _mm_setcsr(state); native_probe((uintptr_t)expected);
          unsigned expected_state=_mm_getcsr();
          _mm_setcsr(state); isa_probe((uintptr_t)actual);
          unsigned actual_state=_mm_getcsr(); _mm_setcsr(saved);
          if(expected_state!=actual_state || memcmp(expected,actual,160)) {
            printf("input=%u state=%08x expected=%08x actual=%08x\n",i,state,expected_state,actual_state);
            for(unsigned j=0;j<160;++j)
              if(expected[j]!=actual[j]) printf("byte%u expected=%02x actual=%02x\n",j,expected[j],actual[j]);
            return 1;
          }
        }
  return 0;
}
)";
#if defined(__linux__)
    if (GSSource) {
      const auto Replace = [&](const std::string &Old, const std::string &New) {
        const auto Position = Text.find(Old);
        EXPECT_NE(Position, std::string::npos);
        if (Position != std::string::npos)
          Text.replace(Position, Old.size(), New);
      };
      Replace("#include <stdint.h>",
              "#include <stdint.h>\n#include <unistd.h>\n#include "
              "<sys/syscall.h>\n#include <asm/prctl.h>");
      Replace("unsigned saved = _mm_getcsr();",
              "unsigned saved = _mm_getcsr(); unsigned long "
              "saved_gs;\nif(syscall(SYS_arch_prctl,ARCH_GET_GS,&saved_gs))"
              "return 77;");
      Replace("native_probe((uintptr_t)expected);",
              "if(syscall(SYS_arch_prctl,ARCH_SET_GS,expected))return "
              "77;\nnative_probe((uintptr_t)expected);");
      Replace("isa_probe((uintptr_t)actual);",
              "if(syscall(SYS_arch_prctl,ARCH_SET_GS,actual))return "
              "77;\nisa_probe((uintptr_t)actual);");
      Replace("unsigned actual_state=_mm_getcsr();",
              "unsigned "
              "actual_state=_mm_getcsr();\nif(syscall(SYS_arch_prctl,ARCH_SET_"
              "GS,saved_gs))return 77;");
    }
#endif
    return Text;
  }
  void check() {
    if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
      GTEST_SKIP()
          << "AVX-enabled x64 host required for complete YMM observation";
    compareFloating(kernel(), sourceElementBytes() == 8);
  }
};
class PackedConvertForms : public PackedConvertFixture,
                           public testing::WithParamInterface<PackedForm> {};
TEST_P(PackedConvertForms, NumericalAndMxcsrMatchNative) {
  Form = GetParam();
  check();
}
INSTANTIATE_TEST_SUITE_P(LegacyAndVex, PackedConvertForms,
                         testing::ValuesIn(packedForms()),
                         [](const testing::TestParamInfo<PackedForm> &I) {
                           return formName(I.param);
                         });
TEST_F(PackedConvertFixture, DiscardedIntegerResultsKeepPrecision) {
  Form = {0, true, 32, true};
  KeepResult = false;
  check();
}
TEST_F(PackedConvertFixture, DiscardedPrecisionResultsKeepInvalid) {
  Form = {3, false, 16, true};
  KeepResult = false;
  check();
}

TEST_F(PackedConvertFixture,
       UnmaskedExceptionsCommitNoResultAndHonorLanePriority) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  for (unsigned Mode = 0; Mode < 5; ++Mode) {
    FaultMode = Mode;
    Form = {Mode == 0   ? 0U
            : Mode == 1 ? 2U
            : Mode == 4 ? 4U
                        : 3U,
            true, 16, true};
    SCOPED_TRACE(Mode);
    compareFloating(kernel(), sourceElementBytes() == 8, true);
  }
}
TEST_F(PackedConvertFixture, LegacyAlignmentFaultPrecedesNumericalEvaluation) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  FaultMode = 6;
  SourceOffset = 65;
  Form = {3, false, 16, true};
  compareFloating(kernel(), true, true);
}
TEST_F(PackedConvertFixture,
       DiscardedMemoryConversionRetainsCompleteFaultingRead) {
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "AVX-enabled x64 host required";
  FaultMode = 5;
  SourceOffset = 63;
  KeepResult = false;
  for (const auto &F :
       {PackedForm{1, false, 16, true}, PackedForm{0, true, 32, true}}) {
    Form = F;
    compareFloating(kernel(), sourceElementBytes() == 8, true);
  }
}
TEST_F(PackedConvertFixture, WideningAndVexMemorySourcesAllowUnalignedAccess) {
  SourceOffset = 65;
  for (const auto &F :
       {PackedForm{1, false, 16, true}, PackedForm{2, false, 16, true},
        PackedForm{3, true, 32, true}, PackedForm{4, true, 32, true}}) {
    Form = F;
    check();
  }
}
TEST_F(PackedConvertFixture, RegisterSourceMayAliasTheCompleteDestination) {
  SourceRegister = 0;
  for (const auto &F :
       {PackedForm{1, false, 16, false}, PackedForm{3, true, 32, false},
        PackedForm{4, true, 16, false}, PackedForm{6, true, 32, false}}) {
    Form = F;
    check();
  }
}
TEST_F(PackedConvertFixture, GSSourcesRetainTheirInstructionAddressSpace) {
#if defined(_WIN32) || defined(__linux__)
  GSSource = true;
  for (const auto &F :
       {PackedForm{1, false, 16, true}, PackedForm{3, true, 32, true}}) {
    Form = F;
    const auto Bytes = kernel();
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Arch::X64));
    for (size_t Offset = 0; Offset < Bytes.size();) {
      DecodedInsn Insn;
      const int Size = Decode.decodeOneForLift(
          Bytes.data() + Offset, Bytes.size() - Offset, Entry + Offset, Insn);
      ASSERT_GT(Size, 0) << formName(Form) << ':' << Offset;
      if (Insn.Raw->bytes[0] == 0x65)
        ASSERT_EQ(Insn.Raw->detail->x86.encoding.disp_size, 4)
            << Insn.Raw->mnemonic << ' ' << Insn.Raw->op_str;
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops))
          << Insn.Raw->mnemonic << ' ' << Insn.Raw->op_str;
      Offset += Size;
    }
    check();
  }
#else
  GTEST_SKIP() << "Native GS source requires Windows/Linux x64";
#endif
}

} // namespace
