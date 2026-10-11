//===- X86_64_FPArithStateAccuracyTests.cpp - SIMD FP completion tests ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/X86FPStateShape.h"

#include <array>

namespace {
struct ArithForm {
  X86FPArithKind Kind;
  bool Double;
  bool Scalar;
  bool Vex;
  unsigned Bytes;
  bool Memory;
  unsigned Topology = 0; // elementwise, horizontal, alternating subtract/add
};

constexpr std::array<uint8_t, 8> Opcodes = {0x58, 0x5c, 0x59, 0x5e,
                                            0,    0x51, 0x5d, 0x5f};
constexpr std::array<const char *, 8> Names = {"Add", "Sub",  "Mul", "Div",
                                               "Fma", "Sqrt", "Min", "Max"};

std::vector<ArithForm> arithForms() {
  std::vector<ArithForm> Forms;
  for (auto Kind :
       {X86FPArithKind::Add, X86FPArithKind::Subtract, X86FPArithKind::Multiply,
        X86FPArithKind::Divide, X86FPArithKind::SquareRoot,
        X86FPArithKind::Minimum, X86FPArithKind::Maximum})
    for (bool Double : {false, true})
      for (bool Memory : {false, true})
        for (const auto [Scalar, Vex, Bytes] :
             {std::tuple{true, false, 16U}, std::tuple{false, false, 16U},
              std::tuple{true, true, 16U}, std::tuple{false, true, 16U},
              std::tuple{false, true, 32U}})
          Forms.push_back({Kind, Double, Scalar, Vex, Bytes, Memory});
  return Forms;
}

std::string arithFormName(const ArithForm &Form) {
  return std::string(Form.Topology == 2 ? "AddSub"
                     : Form.Topology == 1
                         ? (Form.Kind == X86FPArithKind::Add ? "HAdd" : "HSub")
                         : Names[unsigned(Form.Kind)]) +
         (Form.Scalar ? (Form.Double ? "SD" : "SS")
                      : (Form.Double ? "PD" : "PS")) +
         (Form.Vex ? (Form.Bytes == 32 ? "Vex256" : "Vex128") : "Legacy") +
         (Form.Memory ? "Memory" : "Register");
}

class X86FPArithFixture : public X86FPStateFixture {
protected:
  unsigned LeftRegister = 0;
  unsigned DestinationRegister = 0;
  unsigned SourceOffset = 0;
  bool KeepResult = true;
  bool MinimalKernel = false;
  NdMemoryAddressSpace SourceSegment = NdMemoryAddressSpace::Default;
  std::string comparisonDriver(bool IsDouble, bool Declare, bool Fault,
                               bool NativeCall = false) override {
    if (Fault)
      return faultDriver(IsDouble, Declare, NativeCall);
    auto Text = floatingDriver(IsDouble, Declare);
    Text.resize(Text.find("int main(void)"));
    Text += R"(
int main(void) {
  uint32_t saved = _mm_getcsr();
  for (unsigned rounding = 0; rounding < 4; ++rounding)
    for (unsigned environment = 0; environment < 4; ++environment)
      for (unsigned sticky = 0; sticky < 2; ++sticky)
        for (unsigned a = 0; a < sizeof(values)/sizeof(values[0]); ++a)
          for (unsigned b = 0; b < sizeof(values)/sizeof(values[0]); ++b) {
            _Alignas(64) unsigned char expected[160], actual[160];
            for (unsigned i = 0; i < sizeof(expected); ++i)
              expected[i] = (unsigned char)(0x59u + i * 37u);
            for (unsigned lane = 0; lane < 32/sizeof(values[0]); ++lane) {
              unsigned left = (a + lane*3) % (sizeof(values)/sizeof(values[0]));
              unsigned right = (b + lane*7) % (sizeof(values)/sizeof(values[0]));
              memcpy(expected + lane*sizeof(values[0]), &values[left], sizeof(values[0]));
              memcpy(expected + 32 + lane*sizeof(values[0]), &values[right], sizeof(values[0]));
            }
            memcpy(actual, expected, sizeof(expected));
            uint32_t state = 0x1f80 | (rounding << 13) |
              ((environment & 1) ? 0x40 : 0) |
              ((environment & 2) ? 0x8000 : 0) | (sticky ? 0x25 : 0);
            _mm_setcsr(state); native_probe((uintptr_t)expected);
            uint32_t expected_state = _mm_getcsr();
            _mm_setcsr(state); isa_probe((uintptr_t)actual);
            uint32_t actual_state = _mm_getcsr(); _mm_setcsr(saved);
            if (expected_state != actual_state || memcmp(expected, actual, sizeof(expected))) {
              printf("a=%u b=%u incoming=%08x expected=%08x actual=%08x\n",
                     a,b,state,expected_state,actual_state);
              for (unsigned i = 0; i < sizeof(expected); ++i)
                if (expected[i] != actual[i]) printf("byte%u expected=%02x actual=%02x\n",i,expected[i],actual[i]);
              return 1;
            }
          }
  return 0;
}
)";
    if (SourceOffset) {
      const auto Position = Text.find("expected + 32 + lane");
      Text.replace(Position, 15,
                   "expected + " + std::to_string(32 + SourceOffset) + " +");
    }
#if defined(__linux__)
    if (SourceSegment == NdMemoryAddressSpace::X86GS) {
      const auto Replace = [&](const std::string &Old, const std::string &New) {
        const auto Position = Text.find(Old);
        EXPECT_NE(Position, std::string::npos);
        if (Position != std::string::npos)
          Text.replace(Position, Old.size(), New);
      };
      Replace("#include <stdint.h>",
              "#include <stdint.h>\n#include <unistd.h>\n"
              "#include <sys/syscall.h>\n#include <asm/prctl.h>");
      Replace(
          "uint32_t saved = _mm_getcsr();",
          "uint32_t saved = _mm_getcsr(); unsigned long saved_gs;\n"
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

  void check(const ArithForm &Form, bool Fault = false) {
    if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
      GTEST_SKIP() << "native AVX required to observe complete YMM merge state";
    SCOPED_TRACE(arithFormName(Form));
    SCOPED_TRACE(testing::Message()
                 << "src1=" << LeftRegister << " dst=" << DestinationRegister);
    const auto Arg = memoryModRM();
    std::vector<uint8_t> Bytes;
    if (!MinimalKernel)
      Bytes = {0xc5, 0xfe, 0x6f, static_cast<uint8_t>(0x40 | Arg), 64};
    else
      Bytes = {0xc5, 0xfc, 0x57,
               0xc0}; // VXORPS initializes retained upper lanes.
    if (Form.Bytes == 32)
      Bytes.insert(Bytes.end(), {0xc5, 0xfe, 0x6f, Arg});
    else
      Bytes.insert(Bytes.end(), {0xf3, 0x0f, 0x6f, Arg});
    if (LeftRegister)
      Bytes.insert(Bytes.end(),
                   {0xc5, 0xfe, 0x6f,
                    static_cast<uint8_t>(0x40 | (LeftRegister << 3) | Arg),
                    64});
    if (!Form.Memory)
      Bytes.insert(Bytes.end(),
                   {0xc5, 0xfe, 0x6f, static_cast<uint8_t>(0x48 | Arg), 32});
    const unsigned PP = Form.Topology ? (Form.Double ? 1 : 3)
                        : Form.Scalar ? (Form.Double ? 3 : 2)
                                      : (Form.Double ? 1 : 0);
    const unsigned Opcode =
        Form.Topology == 2   ? 0xd0
        : Form.Topology == 1 ? (Form.Kind == X86FPArithKind::Add ? 0x7c : 0x7d)
                             : Opcodes[unsigned(Form.Kind)];
    const bool UnaryPacked =
        Form.Kind == X86FPArithKind::SquareRoot && !Form.Scalar;
    if (SourceSegment != NdMemoryAddressSpace::Default)
      Bytes.push_back(SourceSegment == NdMemoryAddressSpace::X86FS ? 0x64
                                                                   : 0x65);
    if (Form.Vex)
      Bytes.insert(
          Bytes.end(),
          {0xc5,
           static_cast<uint8_t>(
               0x80 | ((UnaryPacked ? 15 : (~LeftRegister & 15)) << 3) |
               (Form.Bytes == 32 ? 4 : 0) | PP),
           static_cast<uint8_t>(Opcode)});
    else {
      if (PP)
        Bytes.push_back(PP == 1 ? 0x66 : PP == 2 ? 0xf3 : 0xf2);
      Bytes.insert(Bytes.end(), {0x0f, static_cast<uint8_t>(Opcode)});
    }
    if (Form.Memory && SourceSegment != NdMemoryAddressSpace::Default) {
#if defined(_WIN32)
      const uint8_t Offset = 0x30;
#else
      const uint8_t Offset =
          SourceSegment == NdMemoryAddressSpace::X86FS ? 0 : 32;
#endif
      Bytes.insert(Bytes.end(),
                   {static_cast<uint8_t>(0x04 | (DestinationRegister << 3)),
                    0x25, Offset, 0, 0, 0});
    } else if (Form.Memory)
      Bytes.insert(Bytes.end(), {static_cast<uint8_t>(
                                     0x40 | (DestinationRegister << 3) | Arg),
                                 static_cast<uint8_t>(32 + SourceOffset)});
    else
      Bytes.push_back(static_cast<uint8_t>(0xc1 | (DestinationRegister << 3)));
    if (KeepResult)
      Bytes.insert(
          Bytes.end(),
          {0xc5, 0xfe, 0x7f,
           static_cast<uint8_t>(0x40 | (DestinationRegister << 3) | Arg), 96});
    Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
    compareFloating(Bytes, Form.Double, Fault);
  }
};

class X86FPArithAccuracy : public X86FPArithFixture,
                           public testing::WithParamInterface<ArithForm> {};

class X86FPArithAliasAccuracy : public X86FPArithFixture {
protected:
  void checkSources(X86FPArithKind Kind) {
    LeftRegister = 2;
    for (bool Double : {false, true})
      for (bool Memory : {false, true})
        for (unsigned Destination : {0U, 1U}) {
          DestinationRegister = Destination;
          check({Kind, Double, true, true, 16, Memory});
        }
  }
};
TEST_F(X86FPArithAliasAccuracy,
       ScalarSubtractUsesDistinctSource1AndAllowsRHSAlias) {
  checkSources(X86FPArithKind::Subtract);
}
TEST_F(X86FPArithAliasAccuracy,
       ScalarMinimumUsesDistinctSource1AndAllowsRHSAlias) {
  checkSources(X86FPArithKind::Minimum);
}
TEST_F(X86FPArithAliasAccuracy,
       ScalarSquareRootMergesSource1AndAllowsRHSAlias) {
  checkSources(X86FPArithKind::SquareRoot);
}
TEST_F(X86FPArithFixture, ScalarMemorySourceAllowsUnalignedAddresses) {
  SourceOffset = 1;
  check({X86FPArithKind::SquareRoot, false, true, false, 16, true});
  check({X86FPArithKind::Maximum, true, true, true, 16, true});
}
TEST_F(X86FPArithFixture, VexPackedMemorySourceAllowsUnalignedAddresses) {
  SourceOffset = 1;
  check({X86FPArithKind::Subtract, false, false, true, 32, true});
  check({X86FPArithKind::Minimum, true, false, true, 16, true});
}

TEST_F(X86FPArithFixture, GSScalarMemoryRetainsItsInstructionAddressSpace) {
  SourceSegment = NdMemoryAddressSpace::X86GS;
  check({X86FPArithKind::SquareRoot, false, true, false, 16, true});
  check({X86FPArithKind::Minimum, true, true, true, 16, true});
}
TEST_F(X86FPArithFixture, FSTLSScalarMemoryRetainsItsInstructionAddressSpace) {
#if defined(__linux__)
  SourceSegment = NdMemoryAddressSpace::X86FS;
  check({X86FPArithKind::Maximum, true, true, true, 16, true});
#else
  GTEST_SKIP() << "native Linux FS TLS source required";
#endif
}

TEST_P(X86FPArithAccuracy, NumericalAndMXCSRCompleteTogether) {
  check(GetParam());
}
INSTANTIATE_TEST_SUITE_P(ArithForms, X86FPArithAccuracy,
                         testing::ValuesIn(arithForms()),
                         [](const testing::TestParamInfo<ArithForm> &Info) {
                           return arithFormName(Info.param);
                         });

std::vector<ArithForm> horizontalForms() {
  std::vector<ArithForm> Forms;
  for (unsigned Topology : {1U, 2U})
    for (auto Kind : {X86FPArithKind::Add, X86FPArithKind::Subtract}) {
      if (Topology == 2 && Kind == X86FPArithKind::Subtract)
        continue;
      for (bool Double : {false, true})
        for (bool Memory : {false, true})
          for (const auto [Vex, Bytes] :
               {std::pair{false, 16U}, std::pair{true, 16U},
                std::pair{true, 32U}})
            Forms.push_back(
                {Kind, Double, false, Vex, Bytes, Memory, Topology});
    }
  return Forms;
}

class X86FPHorizontalAccuracy : public X86FPArithFixture,
                                public testing::WithParamInterface<ArithForm> {
};
TEST_P(X86FPHorizontalAccuracy, NumericalAndMXCSRCompleteTogether) {
  check(GetParam());
}
INSTANTIATE_TEST_SUITE_P(HorizontalForms, X86FPHorizontalAccuracy,
                         testing::ValuesIn(horizontalForms()),
                         [](const testing::TestParamInfo<ArithForm> &Info) {
                           return arithFormName(Info.param);
                         });
TEST_F(X86FPArithFixture, DiscardedHorizontalResultsRetainMXCSR) {
  KeepResult = false;
  for (const auto &Form : horizontalForms())
    if (Form.Bytes == 32 && !Form.Double)
      check(Form);
}
TEST_F(X86FPArithFixture, HorizontalVexMemoryAllowsUnalignedAddresses) {
  SourceOffset = 1;
  for (unsigned Topology : {1U, 2U})
    check({X86FPArithKind::Add, false, false, true, 32, true, Topology});
}
TEST_F(X86FPArithFixture, HorizontalSource1SurvivesDestinationRHSAlias) {
  LeftRegister = 2;
  DestinationRegister = 1;
  for (const auto [Kind, Topology] : {std::pair{X86FPArithKind::Add, 1U},
                                      std::pair{X86FPArithKind::Subtract, 1U},
                                      std::pair{X86FPArithKind::Add, 2U}}) {
    check({Kind, false, false, true, 32, false, Topology});
    check({Kind, true, false, true, 16, false, Topology});
  }
}

TEST(X86FPArithContract, PackedExceptionPriorityMatchesNativeFaultContexts) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  const std::array<std::array<uint32_t, 4>, 5> Left = {
      {{0, 1, 0x3f800000, 0x3f800000},
       {0x7f7fffff, 0x00800000, 1, 0x3f800000},
       {0x3f800000, 0x7f7fffff, 0x00800000, 0x7f800031},
       {0xbf800000, 1, 0x40000000, 0x3f800000},
       {0x7fc00031, 1, 0, 0x3f800000}}};
  const std::array<std::array<uint32_t, 4>, 5> Right = {
      {{0, 0x3f800000, 0, 0x40400000},
       {0x3f000000, 0x40400000, 0x3f800000, 0},
       {1, 0x7f7fffff, 0x00800000, 0x3f800000},
       {0, 0, 0, 0},
       {1, 0x7fc00031, 0x80000000, 0x3f800000}}};
  const std::array<X86FPArithKind, 5> Kinds = {
      X86FPArithKind::Divide, X86FPArithKind::Divide, X86FPArithKind::Multiply,
      X86FPArithKind::SquareRoot, X86FPArithKind::Minimum};
  const std::array<uint32_t, 5> Pre = {7, 6, 3, 3, 1};
  const std::array<uint32_t, 5> Post = {0x20, 0x38, 0x38, 0x20, 0};
  // Five actual DIVPS/MULPS/SQRTPS/MINPS native kernels, each of the64
  // exception-mask combinations: destination stays intact on every fault.
  for (unsigned Group = 0; Group < 5; ++Group)
    for (unsigned Unmask = 0; Unmask < 64; ++Unmask) {
      const uint32_t Incoming = 0x1f80U & ~(Unmask << 7);
      const uint32_t Raised =
          Pre[Group] |
          ((Pre[Group] & Unmask)
               ? 0
               : Post[Group] | (Group == 0 && (Unmask & 16) ? 16U : 0));
      const bool Complete = (Raised & Unmask) == 0;
      SCOPED_TRACE(testing::Message()
                   << "group=" << Group << " unmask=" << Unmask);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setMXCSR(Incoming);
      std::vector<uint8_t> A(16), B(16), Sentinel(16, 0x59);
      std::memcpy(A.data(), Left[Group].data(), 16);
      std::memcpy(B.data(), Right[Group].data(), 16);
      Emulator.setRegisterBytes(10001, A);
      Emulator.setRegisterBytes(10002, B);
      Emulator.setRegisterBytes(10000, Sentinel);
      LowOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.Output = NdVar::reg(10000, 16);
      Op.addInput(NdVar::cst(unsigned(Intrinsic::X86FPArith), 2));
      Op.addInput(NdVar::cst(makeX86FPArithControl(Kinds[Group], false, false,
                                                   false, X86FPRounding::MXCSR),
                             2));
      Op.addInput(NdVar::reg(10001, 16));
      Op.addInput(NdVar::reg(10002, 16));
      Op.addInput(NdVar::cst(0, 16));
      Op.addInput(NdVar::cst(15, 1));
      EXPECT_EQ(Emulator.step(Op), Complete);
      EXPECT_EQ(Emulator.getMXCSR(), Incoming | Raised);
      if (!Complete)
        EXPECT_EQ(Emulator.getRegisterBytes(10000), Sentinel);
    }
}

LowOp arithmeticStateOp(
    unsigned Control, unsigned Bytes, bool Memory = false,
    uint64_t Address = 0x2000, uint32_t State = 0x1f80,
    NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.MemoryAddressSpace = Space;
  Op.Output = NdVar::tmp(0, Bytes + 4);
  const auto Left = x86FPArithStateIsUnary(Control) ? NdVar::cst(0, Bytes)
                                                    : NdVar::reg(10001, Bytes);
  Op.addInput(NdVar::cst(unsigned(Memory ? Intrinsic::X86FPArithMemoryState
                                         : Intrinsic::X86FPArithState),
                         2));
  if (Memory) {
    Op.addInput(NdVar::cst(Address, 8));
    Op.addInput(NdVar::cst(Control, 1));
    Op.addInput(Left);
  } else {
    Op.addInput(NdVar::cst(Control, 1));
    Op.addInput(Left);
    Op.addInput(NdVar::reg(10002, Bytes));
  }
  Op.addInput(NdVar::cst(State, 4));
  return Op;
}

std::vector<uint8_t> arithmeticRepeated(uint64_t Value, unsigned Element,
                                        unsigned Bytes) {
  std::vector<uint8_t> Result(Bytes, 0);
  for (unsigned Offset = 0; Offset < Bytes; Offset += Element)
    for (unsigned Index = 0; Index < Element; ++Index)
      Result[Offset + Index] = uint8_t(Value >> (Index * 8));
  return Result;
}

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
template <typename Scalar>
std::pair<std::array<uint8_t, 16>, uint32_t>
nativeHorizontal(unsigned Control, const uint8_t *A, const uint8_t *B,
                 uint32_t State) {
  typedef Scalar Vector __attribute__((vector_size(16)));
  Vector Left, Right;
  std::memcpy(&Left, A, 16);
  std::memcpy(&Right, B, 16);
  const uint32_t Saved = _mm_getcsr();
#define RUN_HORIZONTAL(PS, PD)                                                 \
  if constexpr (sizeof(Scalar) == 4)                                           \
    __asm__ volatile("ldmxcsr %1\n\t" PS " %2,%0\n\tstmxcsr %1"                \
                     : "+x"(Left), "+m"(State)                                 \
                     : "x"(Right)                                              \
                     : "memory");                                              \
  else                                                                         \
    __asm__ volatile("ldmxcsr %1\n\t" PD " %2,%0\n\tstmxcsr %1"                \
                     : "+x"(Left), "+m"(State)                                 \
                     : "x"(Right)                                              \
                     : "memory")
  if (x86FPArithStateIsAlternating(Control)) {
    RUN_HORIZONTAL("addsubps", "addsubpd");
  } else if ((Control & 7) == unsigned(X86FPArithKind::Subtract)) {
    RUN_HORIZONTAL("hsubps", "hsubpd");
  } else {
    RUN_HORIZONTAL("haddps", "haddpd");
  }
#undef RUN_HORIZONTAL
  std::array<uint8_t, 16> Result;
  std::memcpy(Result.data(), &Left, 16);
  _mm_setcsr(Saved);
  return {Result, State};
}
#endif

TEST(X86FPHorizontalContract, ConcreteStateMatchesNativeRawBitsAndMXCSR) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  if (!llvm::sys::getHostCPUFeatures().lookup("sse3"))
    GTEST_SKIP() << "native SSE3 required";
  constexpr std::array<uint64_t, 18> Singles = {
      0,          0x80000000, 0x3f800000, 0xbf800000, 0x3f800001, 0x3dcccccd,
      1,          0x007fffff, 0x00800000, 0x7f7fffff, 0x7f800000, 0xff800000,
      0x7fc00011, 0x7fc00077, 0x7f800031, 0x7f800071, 0x33800000, 0x40000000};
  constexpr std::array<uint64_t, 18> Doubles = {0,
                                                UINT64_C(0x8000000000000000),
                                                UINT64_C(0x3ff0000000000000),
                                                UINT64_C(0xbff0000000000000),
                                                UINT64_C(0x3ff0000000000001),
                                                UINT64_C(0x3fb999999999999a),
                                                1,
                                                UINT64_C(0x000fffffffffffff),
                                                UINT64_C(0x0010000000000000),
                                                UINT64_C(0x7fefffffffffffff),
                                                UINT64_C(0x7ff0000000000000),
                                                UINT64_C(0xfff0000000000000),
                                                UINT64_C(0x7ff8000000000011),
                                                UINT64_C(0x7ff8000000000077),
                                                UINT64_C(0x7ff0000000000031),
                                                UINT64_C(0x7ff0000000000071),
                                                UINT64_C(0x3ca0000000000000),
                                                UINT64_C(0x4000000000000000)};
  for (unsigned BaseControl : {64U, 65U, 128U, 72U, 73U, 136U}) {
    const bool Double = (BaseControl & 8) != 0;
    const auto &Values = Double ? Doubles : Singles;
    const unsigned Element = Double ? 8 : 4;
    for (unsigned Bytes : {16U, 32U})
      for (bool Memory : {false, true})
        for (unsigned RC = 0; RC < 4; ++RC)
          for (unsigned Environment = 0; Environment < 4; ++Environment)
            for (unsigned Sticky = 0; Sticky < 2; ++Sticky)
              for (unsigned A = 0; A < Values.size(); ++A)
                for (unsigned B = 0; B < Values.size(); ++B) {
                  const uint32_t State =
                      0x1f80 | (RC << 13) | ((Environment & 1) ? 0x40 : 0) |
                      ((Environment & 2) ? 0x8000 : 0) | (Sticky ? 0x25 : 0);
                  const unsigned Control = BaseControl | (Memory ? 32 : 0);
                  SCOPED_TRACE(testing::Message()
                               << "control=" << Control << "bytes=" << Bytes
                               << " a=" << A << " b=" << B
                               << " state=" << State);
                  std::vector<uint8_t> Left(Bytes), Right(Bytes),
                      Expected(Bytes + 4);
                  for (unsigned Lane = 0; Lane < Bytes / Element; ++Lane) {
                    const auto L = Values[(A + Lane * 3) % Values.size()];
                    const auto R = Values[(B + Lane * 7) % Values.size()];
                    std::memcpy(Left.data() + Lane * Element, &L, Element);
                    std::memcpy(Right.data() + Lane * Element, &R, Element);
                  }
                  uint32_t Outgoing = State;
                  for (unsigned Block = 0; Block < Bytes; Block += 16) {
                    const auto Native =
                        Double ? nativeHorizontal<double>(
                                     Control, Left.data() + Block,
                                     Right.data() + Block, Outgoing)
                               : nativeHorizontal<float>(
                                     Control, Left.data() + Block,
                                     Right.data() + Block, Outgoing);
                    std::memcpy(Expected.data() + Block, Native.first.data(),
                                16);
                    Outgoing = Native.second;
                  }
                  std::memcpy(Expected.data() + Bytes, &Outgoing, 4);
                  BinaryImage Image;
                  Image.Arch = Arch::X64;
                  Segment Source;
                  Source.VA = 0x2000;
                  Source.Size = Source.FileSz = Bytes;
                  Source.Flags = SegmentFlags::Readable;
                  Source.Data = Right;
                  Image.Segments.push_back(Source);
                  NdOpEmulator Emulator(Image);
                  Emulator.setStrictMode(true);
                  Emulator.setX86LinearAddressBits(48);
                  Emulator.setRegisterBytes(10001, Left);
                  Emulator.setRegisterBytes(10002, Right);
                  ASSERT_TRUE(Emulator.step(arithmeticStateOp(
                      Control, Bytes, Memory, 0x2000, State)));
                  ASSERT_EQ(Emulator.getRegisterBytes(0), Expected);
                  ASSERT_EQ(Emulator.getMXCSR(), Outgoing);
                }
  }
#else
  GTEST_SKIP() << "native x64 SSE3 oracle requires GCC/Clang";
#endif
}

TEST(X86FPHorizontalContract,
     ScalarAndConflictingTopologiesRefuseWithoutWrites) {
  for (unsigned Control : {80U, 192U, 66U, 129U, 152U})
    for (bool Memory : {false, true}) {
      BinaryImage Image;
      Image.Arch = Arch::X64;
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setMXCSR(0x1fa5);
      const std::vector<uint8_t> Sentinel(20, 0x59);
      Emulator.setRegisterBytes(10000, Sentinel);
      auto Op = arithmeticStateOp(Control, 16, Memory);
      Op.Output = NdVar::reg(10000, 20);
      const auto Id = Memory ? Intrinsic::X86FPArithMemoryState
                             : Intrinsic::X86FPArithState;
      EXPECT_FALSE(
          x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X64)));
      EXPECT_FALSE(Emulator.step(Op));
      EXPECT_EQ(Emulator.getRegisterBytes(10000), Sentinel);
      EXPECT_EQ(Emulator.getMXCSR(), 0x1fa5U);
    }
}

TEST(X86FPArithContract, SquareRootUsesRHSAndDoesNotReadTheNumericalDummy) {
  for (bool Memory : {false, true})
    for (bool Double : {false, true})
      for (bool Scalar : {false, true})
        for (unsigned Bytes : {4U, 8U, 16U, 32U}) {
          const unsigned Element = Double ? 8 : 4;
          if (Scalar ? Bytes != Element : Bytes < 16)
            continue;
          const unsigned Control = unsigned(X86FPArithKind::SquareRoot) |
                                   (Double ? 8 : 0) | (Scalar ? 16 : 0) |
                                   (Memory && !Scalar ? 32 : 0);
          BinaryImage Image;
          Image.Arch = Arch::X64;
          Segment Segment;
          Segment.VA = 0x2000;
          Segment.Size = Segment.FileSz = Bytes;
          Segment.Flags = SegmentFlags::Readable;
          Segment.Data = arithmeticRepeated(
              Double ? UINT64_C(0x4010000000000000) : UINT64_C(0x40800000),
              Element, Bytes);
          Image.Segments.push_back(Segment);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setX86LinearAddressBits(48);
          Emulator.setMXCSR(0x3f80);
          Emulator.setRegisterBytes(10002, Segment.Data);
          const auto Op = arithmeticStateOp(Control, Bytes, Memory);
          ASSERT_TRUE(Emulator.step(Op));
          auto Expected = arithmeticRepeated(
              Double ? UINT64_C(0x4000000000000000) : UINT64_C(0x40000000),
              Element, Bytes);
          const uint32_t State = 0x1f80;
          Expected.resize(Bytes + 4);
          std::memcpy(Expected.data() + Bytes, &State, 4);
          EXPECT_EQ(Emulator.getRegisterBytes(0), Expected);
          EXPECT_EQ(Emulator.getMXCSR(), State);
        }
}

TEST(X86FPArithContract, ValueAndMemoryControlsRequireExactOperandRoles) {
  for (bool Memory : {false, true})
    for (bool Double : {false, true})
      for (bool Scalar : {false, true})
        for (unsigned Bytes : {4U, 8U, 16U, 32U}) {
          if (Scalar ? Bytes != (Double ? 8 : 4) : Bytes < 16)
            continue;
          const unsigned Control = unsigned(X86FPArithKind::SquareRoot) |
                                   (Double ? 8 : 0) | (Scalar ? 16 : 0) |
                                   (Memory && !Scalar ? 32 : 0);
          auto Op = arithmeticStateOp(Control, Bytes, Memory);
          const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
          const auto Good = x86FPStateLowShape(Op, Arch::X64);
          ASSERT_TRUE(x86FPStateShapeIsValid(Id, Good));
          for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
            auto Bad = Op;
            switch (Mutation) {
            case 0:
              Bad.Inputs[0] = NdVar::cst(unsigned(Id) + 0x10000, 2);
              break;
            case 1:
              Bad.Inputs[Memory ? 2 : 1] = NdVar::cst(Control | 0x100, 1);
              break;
            case 2:
              Bad.Inputs[Memory ? 2 : 1] = NdVar::reg(99, 1);
              break;
            case 3:
              Bad.Inputs[Memory ? 2 : 1].Size = 2;
              break;
            case 4:
              Bad.Inputs[4].Size = 8;
              break;
            case 5:
              Bad.Inputs[Memory ? 3 : 2] = NdVar::reg(10001, Bytes);
              break;
            case 6:
              Bad.Inputs[3].Size = Bytes + 1;
              break;
            case 7:
              Bad.Output.Size = Bytes;
              break;
            case 8:
              Bad.NumInputs = 6;
              break;
            case 9:
              Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
              break;
            case 10:
              Bad.Inputs[Memory ? 2 : 1] = NdVar::cst(
                  (Control & ~7U) | unsigned(X86FPArithKind::FusedMultiplyAdd),
                  1);
              break;
            case 11:
              Bad.Output = NdVar::cst(0, Bytes + 4);
              break;
            }
            EXPECT_FALSE(
                x86FPStateShapeIsValid(Id, x86FPStateLowShape(Bad, Arch::X64)))
                << Mutation;
            BinaryImage Image;
            Image.Arch = Arch::X64;
            NdOpEmulator Emulator(Image);
            Emulator.setStrictMode(true);
            Emulator.setMXCSR(0x3f80);
            EXPECT_FALSE(Emulator.step(Bad)) << Mutation;
            EXPECT_FALSE(Emulator.getRegisterBytes(0));
            EXPECT_EQ(Emulator.getMXCSR(), 0x3f80U);
          }
          auto Bad = Good;
          Bad.TargetArch = Arch::AArch64;
          EXPECT_FALSE(x86FPStateShapeIsValid(Id, Bad));
          Bad = Good;
          Bad.HasAuxiliaryOutputs = true;
          EXPECT_FALSE(x86FPStateShapeIsValid(Id, Bad));
          EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Good, 0, Bytes),
                    Scalar ? Bytes : 0U);
          EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Good, 1, Bytes), 0U);
          EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Good, 0, Bytes - 1), 0U);
          EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Good, Bytes, 4), 0U);
        }
}

TEST(X86FPArithContract, HighAndMedAdaptersAuthenticateDummyAndArchitecture) {
  for (bool Memory : {false, true}) {
    const auto Id =
        Memory ? Intrinsic::X86FPArithMemoryState : Intrinsic::X86FPArithState;
    const unsigned Control = unsigned(X86FPArithKind::SquareRoot) | 16;
    MedOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = {
        .Kind = MedVar::Temp, .TheArch = Arch::X64, .Id = 1, .Size = 8};
    Op.addInput(MedVar::makeConst(unsigned(Id), 2));
    Op.addInput(MedVar::makeConst(Memory ? 0x2000 : Control, Memory ? 8 : 1));
    Op.addInput(MedVar::makeConst(Memory ? Control : 0, Memory ? 1 : 4));
    Op.addInput(MedVar::makeConst(0, 4));
    Op.addInput(MedVar::makeConst(0x1f80, 4));
    ASSERT_TRUE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Op)));
    const auto OriginalCount = Op.NumInputs;
    Op.NumInputs = Op.Inputs.size() + 1;
    EXPECT_FALSE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Op)));
    Op.NumInputs = OriginalCount;
    const auto LeftIndex = Memory ? 3 : 2;
    Op.Inputs[LeftIndex] = {
        .Kind = MedVar::Param, .TheArch = Arch::X64, .Id = 2, .Size = 4};
    EXPECT_FALSE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Op)));
    Op.Inputs[LeftIndex] = MedVar::makeConst(0, 4);
    Op.Inputs[4].TheArch = Arch::ARM;
    EXPECT_FALSE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Op)));
    Op.Inputs[4].TheArch = Arch::Unknown;
    Op.IntrinsicOutputs.push_back({.Kind = MedVar::Temp, .Id = 3, .Size = 4});
    EXPECT_FALSE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Op)));
    auto Call = HighExpr::makeCall(
        "", 0,
        {HighExpr::makeConst(Memory ? 0x2000 : Control, Memory ? 8 : 1),
         HighExpr::makeConst(Memory ? Control : 0, Memory ? 1 : 4),
         HighExpr::makeConst(0, 4), HighExpr::makeConst(0x1f80, 4)});
    Call->IntrinsicId = Id;
    Call->Type = NdType::makeInt(8, false);
    ASSERT_TRUE(
        x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
    Call->Operands[Memory ? 2 : 1]->ConstVal = 1;
    EXPECT_FALSE(
        x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
    Call->Operands[Memory ? 2 : 1]->ConstVal = 0;
    Call->IsIndirectCall = true;
    EXPECT_FALSE(
        x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
    Call->IsIndirectCall = false;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>();
    EXPECT_FALSE(
        x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
  }
}

TEST(X86FPArithContract,
     InstructionMemoryTopologyCannotBecomeSAEOrAuthorizeReads) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (auto Space :
         {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
          NdMemoryAddressSpace::X86GS})
      for (unsigned Control : {3U, 35U}) {
        BinaryImage Image;
        Image.Arch = Target;
        Segment Source;
        Source.VA = 0x2000;
        Source.Size = Source.FileSz = 32;
        Source.Flags = SegmentFlags::Readable;
        Source.Data = arithmeticRepeated(0x40400000, 4, 32);
        Image.Segments.push_back(Source);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        if (Target == Arch::X64)
          Emulator.setX86LinearAddressBits(48);
        if (Space != NdMemoryAddressSpace::Default)
          Emulator.setMemoryAddressSpaceBase(Space, 0x1000);
        Emulator.setRegisterBytes(10001, arithmeticRepeated(0x3f800000, 4, 16));
        const uint64_t Offset =
            Space == NdMemoryAddressSpace::Default ? 0x2000 : 0x1000;
        const uint64_t Address =
            Offset | (Target == Arch::X86 ? UINT64_C(0x100000000) : 0);
        const auto Op =
            arithmeticStateOp(Control, 16, true, Address, 0x1f80, Space);
        ASSERT_TRUE(Emulator.step(Op));
        EXPECT_EQ(Emulator.getMXCSR(), 0x1fa0U);
        Emulator.reset();
        Emulator.setMXCSR(0x3f80);
        Emulator.setRegisterBytes(10001, arithmeticRepeated(0x3f800000, 4, 16));
        Image.Segments[0].Flags = SegmentFlags::Writable;
        EXPECT_FALSE(Emulator.step(Op));
        EXPECT_FALSE(Emulator.getRegisterBytes(0));
        EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
      }
}

TEST(X86FPArithContract,
     RawEncodingAndDescriptorsAgreeForEveryElementwiseKind) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (unsigned Prefix : {0U, 1U, 2U})
        for (bool Scalar : {false, true})
          for (bool Double : {false, true})
            for (bool Memory : {false, true})
              for (unsigned Kind : {0U, 1U, 2U, 3U, 5U, 6U, 7U})
                for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
                  SCOPED_TRACE(testing::Message()
                               << "arch=" << unsigned(Target)
                               << " strict=" << Strict << " prefix=" << Prefix
                               << " scalar=" << Scalar << " double=" << Double
                               << " memory=" << Memory << " kind=" << Kind
                               << " mutation=" << Mutation);
                  Decoder Decode;
                  ASSERT_TRUE(Decode.init(Target));
                  Decode.setStrict(Strict);
                  const unsigned PP =
                      Scalar ? (Double ? 3 : 2) : (Double ? 1 : 0);
                  const bool Unary = Kind == 5;
                  const bool HasVvvv = Prefix && (!Unary || Scalar);
                  std::vector<uint8_t> Bytes;
                  if (Prefix == 1)
                    Bytes = {0xc5,
                             uint8_t(0x80 | (HasVvvv ? 0x68 : 0x78) | PP)};
                  else if (Prefix == 2)
                    Bytes = {0xc4, 0xe1, uint8_t((HasVvvv ? 0x68 : 0x78) | PP)};
                  else {
                    if (PP)
                      Bytes.push_back(PP == 1 ? 0x66 : PP == 2 ? 0xf3 : 0xf2);
                    Bytes.push_back(0x0f);
                  }
                  Bytes.insert(Bytes.end(),
                               {Opcodes[Kind], uint8_t(Memory ? 0x00 : 0xc1)});
                  DecodedInsn Insn;
                  ASSERT_EQ(
                      Decode.decodeOne(Bytes.data(), Bytes.size(), Entry, Insn),
                      Bytes.size());
                  auto &Detail = Insn.Raw->detail->x86;
                  const unsigned SourceIndex = HasVvvv ? 2 : 1;
                  if (Mutation == 0) {
                    std::vector<LowOp> Ops;
                    ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops));
                    unsigned Found = 0;
                    for (const auto &Op : Ops)
                      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                          Op.Inputs[0].isConst()) {
                        const auto Id =
                            static_cast<Intrinsic>(Op.Inputs[0].Offset);
                        if (isX86FPArithStateIntrinsic(Id) ||
                            isX86ScalarFPStateIntrinsic(Id)) {
                          ++Found;
                          EXPECT_TRUE(x86FPStateShapeIsValid(
                              Id, x86FPStateLowShape(Op, Target)));
                        }
                      }
                    EXPECT_EQ(Found, 1U);
                    continue;
                  }
                  switch (Mutation) {
                  case 1:
                    Detail.operands[0].size = 32;
                    break;
                  case 2:
                    Detail.operands[0].reg = X86_REG_XMM3;
                    break;
                  case 3:
                    Detail.operands[SourceIndex].size =
                        Detail.operands[SourceIndex].size == 8 ? 4 : 8;
                    break;
                  case 4:
                    Insn.Raw->bytes[Detail.encoding.modrm_offset - 1] = 0x50;
                    break;
                  case 5:
                    Insn.Raw->size = 16;
                    break;
                  case 6:
                    Detail.op_count++;
                    break;
                  case 7:
                    Detail.avx_sae = true;
                    break;
                  }
                  std::vector<LowOp> Ops;
                  EXPECT_THROW(Decode.liftToLow(Insn, Ops),
                               UnliftedInstruction);
                }
}

class X86FPArithFaultAccuracy : public X86FPArithFixture {
protected:
  unsigned FaultMode = 0; // numerical, alignment, protected page
  unsigned Group = 0;
  unsigned Unmask = 1;
  unsigned ExpectedFlags = 7;
  unsigned SourceBytes = 16;

  std::string comparisonDriver(bool Double, bool Declare, bool Fault,
                               bool NativeCall = false) override {
    if (!Fault)
      return X86FPArithFixture::comparisonDriver(Double, Declare, Fault,
                                                 NativeCall);
    std::string Text = R"(
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
extern void native_probe(uintptr_t);
static _Alignas(64) unsigned char storage[160];
static unsigned char *actual=storage;
static uintptr_t source_first,source_last;
)";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    const uint32_t Incoming = 0x1f80U & ~(Unmask << 7);
    Text += "enum { fault_mode=" + std::to_string(FaultMode) +
            ", incoming=" + std::to_string(Incoming) +
            ", expected_csr=" + std::to_string(Incoming | ExpectedFlags) +
            " };\n";
    Text += R"(
static int output_untouched(void) {
  for(unsigned i=96;i<128;++i)
    if(actual[i]!=(unsigned char)(0x59u+i*37u)) return 0;
  return 1;
}
static int fault_matches(uint32_t csr,uintptr_t address,int memory_fault) {
  if(csr!=expected_csr || memory_fault!=(fault_mode!=0)) return 0;
  if(fault_mode==2) return address>=source_first && address<source_last;
  return output_untouched();
}
#if defined(_WIN32)
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *info) {
  DWORD code=info->ExceptionRecord->ExceptionCode;
  int memory_fault=code==EXCEPTION_ACCESS_VIOLATION;
  if(memory_fault || code==EXCEPTION_FLT_INVALID_OPERATION ||
     code==EXCEPTION_FLT_DENORMAL_OPERAND || code==EXCEPTION_FLT_DIVIDE_BY_ZERO ||
     code==EXCEPTION_FLT_OVERFLOW || code==EXCEPTION_FLT_UNDERFLOW ||
     code==EXCEPTION_FLT_INEXACT_RESULT || code==STATUS_FLOAT_MULTIPLE_FAULTS ||
     code==STATUS_FLOAT_MULTIPLE_TRAPS) {
    uintptr_t address=memory_fault && info->ExceptionRecord->NumberParameters>1 ?
        (uintptr_t)info->ExceptionRecord->ExceptionInformation[1] : 0;
    ExitProcess(fault_matches(info->ContextRecord->MxCsr,address,memory_fault)?0:1);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void on_fault(int number,siginfo_t *info,void *opaque) {
  ucontext_t *context=(ucontext_t *)opaque;
  int memory_fault=number==SIGSEGV || number==SIGBUS;
  _exit(fault_matches(context->uc_mcontext.fpregs->mxcsr,
                     (uintptr_t)info->si_addr,memory_fault)?0:1);
}
#endif
int main(void) {
#if defined(_WIN32)
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  if(!AddVectoredExceptionHandler(1,on_fault)) return 77;
#else
  struct sigaction action={0};action.sa_sigaction=on_fault;action.sa_flags=SA_SIGINFO;
  if(sigaction(SIGFPE,&action,0) || sigaction(SIGSEGV,&action,0) ||
     sigaction(SIGBUS,&action,0)) return 77;
#endif
  if(fault_mode==2) {
#if defined(_WIN32)
    SYSTEM_INFO system;GetSystemInfo(&system);
    size_t page=system.dwPageSize;
    unsigned char *arena=VirtualAlloc(0,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    DWORD old;
    if(!arena || !VirtualProtect(arena+page,page,PAGE_NOACCESS,&old)) return 77;
#else
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *arena=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(arena==MAP_FAILED || mprotect(arena+page,page,PROT_NONE)) return 77;
#endif
    if(page<160) return 77;
    memset(arena,0,page);
    actual=arena+page-36;
    source_first=(uintptr_t)(actual+32);
)";
    Text +=
        "    source_last=source_first+" + std::to_string(SourceBytes) + ";\n";
    Text += R"(
  } else {
    for(unsigned i=0;i<160;++i) actual[i]=(unsigned char)(0x59u+i*37u);
)";
    if (Double)
      Text += R"(
    const uint64_t a[5][4]={
      {0,1,UINT64_C(0x3ff0000000000000),UINT64_C(0x3ff0000000000000)},
      {UINT64_C(0x7fefffffffffffff),UINT64_C(0x0010000000000000),1,UINT64_C(0x3ff0000000000000)},
      {UINT64_C(0x3ff0000000000000),UINT64_C(0x7fefffffffffffff),UINT64_C(0x0010000000000000),UINT64_C(0x7ff0000000000031)},
      {UINT64_C(0xbff0000000000000),1,UINT64_C(0x4000000000000000),UINT64_C(0x3ff0000000000000)},
      {UINT64_C(0x7ff8000000000031),1,0,UINT64_C(0x3ff0000000000000)}};
    const uint64_t b[5][4]={
      {0,UINT64_C(0x3ff0000000000000),0,UINT64_C(0x4008000000000000)},
      {UINT64_C(0x3fe0000000000000),UINT64_C(0x4008000000000000),UINT64_C(0x3ff0000000000000),0},
      {1,UINT64_C(0x7fefffffffffffff),UINT64_C(0x0010000000000000),UINT64_C(0x3ff0000000000000)},
      {0,0,0,0},{1,UINT64_C(0x7ff8000000000031),UINT64_C(0x8000000000000000),UINT64_C(0x3ff0000000000000)}};
)";
    else
      Text += R"(
    const uint32_t a[5][4]={
      {0,1,0x3f800000,0x3f800000},{0x7f7fffff,0x00800000,1,0x3f800000},
      {0x3f800000,0x7f7fffff,0x00800000,0x7f800031},
      {0xbf800000,1,0x40000000,0x3f800000},{0x7fc00031,1,0,0x3f800000}};
    const uint32_t b[5][4]={
      {0,0x3f800000,0,0x40400000},{0x3f000000,0x40400000,0x3f800000,0},
      {1,0x7f7fffff,0x00800000,0x3f800000},{0,0,0,0},
      {1,0x7fc00031,0x80000000,0x3f800000}};
)";
    Text += "    unsigned group=" + std::to_string(Group) +
            ";\n"
            "    for(unsigned lane=0;lane<32/sizeof(a[0][0]);++lane) {\n"
            "      "
            "memcpy(actual+lane*sizeof(a[0][0]),&a[group][lane%4],sizeof(a[0]["
            "0]));\n"
            "      memcpy(actual+" +
            std::to_string(32 + SourceOffset) +
            "+lane*sizeof(a[0][0]),\n"
            "             "
            "group==3?&a[group][lane%4]:&b[group][lane%4],sizeof(a[0][0]));\n"
            "    }\n  }\n  _mm_setcsr(incoming);\n";
    Text += NativeCall ? "  native_probe((uintptr_t)actual);\n"
                       : "  isa_probe((uintptr_t)actual);\n";
    Text +=
        "  _mm_setcsr(0x1f80); return 99; /* missing required fault */\n}\n";
    return Text;
  }
  void runFault(const ArithForm &Form, unsigned Case, unsigned Mask,
                unsigned Flags) {
    MinimalKernel = true;
    Group = Case;
    Unmask = Mask;
    ExpectedFlags = Flags;
    SourceBytes = Form.Scalar ? (Form.Double ? 8 : 4) : Form.Bytes;
    check(Form, true);
  }
};

TEST_F(X86FPArithFaultAccuracy, PackedInvalidPrecedesOtherLanePrecision) {
  for (bool Memory : {false, true})
    runFault({X86FPArithKind::Divide, false, false, false, 16, Memory}, 0, 1,
             7);
}
TEST_F(X86FPArithFaultAccuracy,
       PackedOverflowRetainsAllCompletedExceptionEvidence) {
  for (bool Memory : {false, true})
    runFault({X86FPArithKind::Multiply, false, false, true, 32, Memory}, 2, 8,
             0x3b);
}
TEST_F(X86FPArithFaultAccuracy,
       PackedUnderflowRetainsAllCompletedExceptionEvidence) {
  for (bool Memory : {false, true})
    runFault({X86FPArithKind::Divide, false, false, true, 32, Memory}, 1, 16,
             0x3e);
}
TEST_F(X86FPArithFaultAccuracy,
       DiscardedPackedSquareRootRetainsUnmaskedPrecision) {
  KeepResult = false;
  for (bool Memory : {false, true})
    runFault({X86FPArithKind::SquareRoot, false, false, true, 16, Memory}, 3,
             32, 0x23);
}
TEST_F(X86FPArithFaultAccuracy, PackedMinimumRetainsInvalidOnQuietNaN) {
  for (bool Memory : {false, true})
    runFault({X86FPArithKind::Minimum, false, false, true, 16, Memory}, 4, 1,
             1);
}
TEST_F(X86FPArithFaultAccuracy,
       DoubleSquareRootFaultRetainsDenormalAndInvalid) {
  for (bool Memory : {false, true})
    runFault({X86FPArithKind::SquareRoot, true, false, false, 16, Memory}, 3, 1,
             3);
}
TEST_F(X86FPArithFaultAccuracy,
       LegacyPackedMisalignmentPrecedesFloatingPointEvaluation) {
  FaultMode = 1;
  SourceOffset = 1;
  runFault({X86FPArithKind::Multiply, false, false, false, 16, true}, 2, 0, 0);
}
TEST_F(X86FPArithFaultAccuracy,
       DiscardedResultStillRaisesTheLegacyAlignmentFault) {
  FaultMode = 1;
  SourceOffset = 1;
  KeepResult = false;
  runFault({X86FPArithKind::SquareRoot, false, false, false, 16, true}, 3, 0,
           0);
}
TEST_F(X86FPArithFaultAccuracy,
       CrossPageMemorySourcesRequireTheCompleteVector) {
  FaultMode = 2;
  runFault({X86FPArithKind::Divide, false, false, true, 16, true}, 0, 0, 0);
  runFault({X86FPArithKind::SquareRoot, true, false, true, 32, true}, 3, 0, 0);
}
TEST_F(X86FPArithFaultAccuracy, DiscardedVectorStillReadsTheProtectedSource) {
  FaultMode = 2;
  KeepResult = false;
  runFault({X86FPArithKind::Multiply, false, false, true, 32, true}, 2, 0, 0);
}
TEST_F(X86FPArithFaultAccuracy, HorizontalInvalidPrecedesOtherLanePrecision) {
  for (const auto [Kind, Topology] : {std::pair{X86FPArithKind::Add, 1U},
                                      std::pair{X86FPArithKind::Subtract, 1U},
                                      std::pair{X86FPArithKind::Add, 2U}})
    for (bool Memory : {false, true})
      runFault({Kind, false, false, true, 32, Memory, Topology}, 2, 1, 3);
}
TEST_F(X86FPArithFaultAccuracy, DiscardedHorizontalStillRaisesUnmaskedInvalid) {
  KeepResult = false;
  for (unsigned Topology : {1U, 2U})
    runFault({X86FPArithKind::Add, false, false, true, 16, true, Topology}, 2,
             1, 3);
}
TEST_F(X86FPArithFaultAccuracy,
       LegacyHorizontalMisalignmentPrecedesEvaluation) {
  FaultMode = 1;
  SourceOffset = 1;
  KeepResult = false;
  for (unsigned Topology : {1U, 2U})
    runFault({X86FPArithKind::Add, false, false, false, 16, true, Topology}, 2,
             0, 0);
}
TEST_F(X86FPArithFaultAccuracy,
       DiscardedHorizontalReadsTheCompleteProtectedSource) {
  FaultMode = 2;
  KeepResult = false;
  for (unsigned Topology : {1U, 2U})
    runFault({X86FPArithKind::Add, false, false, true, 32, true, Topology}, 2,
             0, 0);
}
TEST_F(X86FPArithFixture, DiscardedPackedNumericalResultsRetainMXCSRFlags) {
  KeepResult = false;
  check({X86FPArithKind::Add, false, false, false, 16, false});
  check({X86FPArithKind::Multiply, true, false, true, 32, true});
}

TEST(X86FPArithContract,
     OwnedAssemblyAuthenticatesRolesStateAddressAndEffects) {
  for (unsigned Form = 0; Form < 5; ++Form)
    for (unsigned Mutation = 0; Mutation < 14; ++Mutation) {
      SCOPED_TRACE(testing::Message()
                   << "form=" << Form << " mutation=" << Mutation);
      llvm::LLVMContext Context;
      llvm::Module Module("arith-owned", Context);
      llvm::IRBuilder<> Builder(Context);
      const bool Unary = Form == 1;
      const bool Memory = Form == 2 || Form == 4;
      const unsigned Control = Form == 3   ? 64
                               : Form == 4 ? 160
                               : Unary     ? 29
                               : Memory    ? 32
                                           : 3;
      const unsigned Layout = x86FPRoundStateLayout(
          Unary    ? 8
          : Memory ? 32
                   : 16,
          Control, 0,
          Memory ? NdMemoryAddressSpace::X86GS : NdMemoryAddressSpace::Default);
      auto *Type = x86FPArithStateLLVMType(Context, Layout);
      llvm::Type *RHS =
          Memory ? static_cast<llvm::Type *>(Builder.getPtrTy(256)) : Type;
      llvm::Type *LHS = Type;
      llvm::Type *State = Builder.getPtrTy();
      if (Mutation == 1)
        RHS = Memory ? static_cast<llvm::Type *>(Builder.getPtrTy())
                     : Builder.getInt64Ty();
      if (Mutation == 2)
        State = Builder.getPtrTy(257);
      if (Mutation == 3)
        State = Builder.getInt64Ty();
      if (Mutation == 4)
        LHS = Builder.getInt64Ty();
      std::vector<llvm::Type *> Inputs;
      if (!Unary)
        Inputs.push_back(LHS);
      Inputs.push_back(RHS);
      Inputs.push_back(State);
      if (Mutation == 5)
        Inputs.pop_back();
      std::string Text = x86FPArithStateAsm(Layout, Memory);
      if (Mutation == 6)
        Text += "\n\tnop";
      std::string Constraints = x86FPArithStateConstraints(Layout, Memory);
      if (Mutation == 7)
        Constraints.replace(0, 3, "=x");
      if (Mutation == 8)
        Constraints.resize(Constraints.find(",~{memory}"));
      auto *Fn = llvm::Function::Create(
          llvm::FunctionType::get(Type, Inputs, false),
          llvm::Function::ExternalLinkage, "arith", Module);
      Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Fn));
      auto *Asm = llvm::InlineAsm::get(
          Fn->getFunctionType(), Text, Constraints, Mutation != 9,
          Mutation == 10,
          Mutation == 11 ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT,
          Mutation == 12);
      std::vector<llvm::Value *> Arguments;
      for (auto &Argument : Fn->args())
        Arguments.push_back(&Argument);
      auto *Call = Builder.CreateCall(Asm, Arguments);
      if (Mutation != 13)
        Call->setMetadata(X86FPStateAsmMetadata,
                          llvm::MDNode::get(Context, {}));
      Builder.CreateRet(Call);
      if (!Mutation || (Unary && Mutation == 4))
        ASSERT_EQ(classifyX86FPStateAsm(*Call),
                  std::make_optional(
                      std::pair{Memory ? Intrinsic::X86FPArithMemoryState
                                       : Intrinsic::X86FPArithState,
                                Layout}));
      else if (Mutation == 13)
        EXPECT_FALSE(classifyX86FPStateAsm(*Call));
      else
        EXPECT_DEATH(classifyX86FPStateAsm(*Call),
                     "invalid x86 FP state assembly");
    }
}

class X86FPArithReturnAccuracy : public X86FPStateFixture {
protected:
  void compareFloatingReturn(bool IsDouble, uint8_t Opcode, bool Vex) {
    if (!nativeX64())
      GTEST_SKIP() << "native x86_64 host required";
    if (Vex && !llvm::sys::getHostCPUFeatures().lookup("avx"))
      GTEST_SKIP() << "native AVX required for the VEX scalar return probe";
    const std::vector<uint8_t> Bytes =
        Vex ? std::vector<uint8_t>{0xc5,
                                   static_cast<uint8_t>(IsDouble ? 0xfb : 0xfa),
                                   Opcode, 0xc0, 0xc3}
            : std::vector<uint8_t>{static_cast<uint8_t>(IsDouble ? 0xf2 : 0xf3),
                                   0x0f, Opcode, 0xc0, 0xc3};
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
          ASSERT_EQ(Result.HighFuncs[0].Params[0].Type->Size, IsDouble ? 8 : 4);
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

TEST_F(X86FPArithReturnAccuracy,
       ScalarSquareRootRetainsFloatingReturnAndMXCSR) {
  for (bool Double : {false, true})
    compareFloatingReturn(Double, 0x51, false);
}
TEST_F(X86FPArithReturnAccuracy, ScalarMinimumAndMaximumRetainFloatingReturns) {
  for (bool Double : {false, true})
    for (uint8_t Opcode : {0x5d, 0x5f})
      compareFloatingReturn(Double, Opcode, false);
}
TEST_F(X86FPArithReturnAccuracy,
       VexScalarSubtractRetainsFloatingReturnAndMXCSR) {
  for (bool Double : {false, true})
    compareFloatingReturn(Double, 0x5c, true);
}

TEST_F(X86FPArithFixture,
       ObservedMergedUpperLanesPreventScalarParameterRecovery) {
  for (bool NoOpt : {false, true}) {
    const auto Arg = memoryModRM();
    const std::vector<uint8_t> Bytes = {
        0xc5, 0xfa, 0x5c, 0xc0, // VSUBSS XMM0,XMM0,XMM0
        0xc5, 0xfe, 0x7f, Arg, // VMOVDQU [arg],YMM0 observes merged high lanes.
        0x31, 0xc0, 0xc3};
    auto Image = image(Bytes);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.NoOpt = NoOpt;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry};
    auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.MedFuncs.size(), 1U);
    EXPECT_FALSE(Result.MedFuncs.front().FPParamScalarBytes.count(0x100));
  }
}

class X86FPArithStackAccuracy : public X86FPStateFixture {
protected:
  void compareStackArguments(bool IsDouble, bool Declared) {
    if (!nativeX64())
      GTEST_SKIP() << "native x86_64 host required";
    const unsigned Registers = hostFormat() == BinaryFormat::COFF ? 4 : 8;
    const unsigned Offset = hostFormat() == BinaryFormat::COFF ? 40 : 8;
    const std::string Scalar = IsDouble ? "double" : "float";
    std::vector<uint8_t> Bytes;
    for (unsigned I = 1; I < Registers; ++I)
      Bytes.insert(Bytes.end(), {static_cast<uint8_t>(IsDouble ? 0xf2 : 0xf3),
                                 0x0f, 0x58, static_cast<uint8_t>(0xc0 | I)});
    Bytes.insert(Bytes.end(),
                 {static_cast<uint8_t>(IsDouble ? 0xf2 : 0xf3), 0x0f, 0x58,
                  0x44, 0x24, static_cast<uint8_t>(Offset), 0xc3});
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
        Scalar + " native_probe(" + Signature + ");\n" +
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
              Scalar + " expected = native_probe(" + Arguments +
              ");\nuint32_t expected_state = _mm_getcsr();\n"
              "_mm_setcsr(state);\n" +
              Scalar + " actual = isa_probe(" + Arguments + R"();
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
        Hint.ReturnType = NdType::makeFloat(IsDouble ? 8 : 4);
        const auto &TRI = getTargetRegInfo(Arch::X64);
        for (unsigned I = 0; I <= Registers; ++I) {
          SourceParameterTypeHint Parameter;
          Parameter.Name = "arg" + std::to_string(I);
          Parameter.Type = Hint.ReturnType;
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
        Hint.ReturnLocation.ValueBytes = IsDouble ? 8 : 4;
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

TEST_F(X86FPArithStackAccuracy, DeclaredFloatStackArgumentBindsItsSourceABI) {
  compareStackArguments(false, true);
}
TEST_F(X86FPArithStackAccuracy, DeclaredDoubleStackArgumentBindsItsSourceABI) {
  compareStackArguments(true, true);
}

} // namespace
