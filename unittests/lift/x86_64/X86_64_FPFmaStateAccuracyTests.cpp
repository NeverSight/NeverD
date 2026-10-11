//===- X86_64_FPFmaStateAccuracyTests.cpp - Native FMA completion tests
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include <array>
#include <type_traits>

namespace {
class X86FPFmaStateAccuracy : public X86FPStateFixture {
protected:
  void scalar(bool Double, unsigned Order, bool Negate, bool Keep) {
    if (!llvm::sys::getHostCPUFeatures().lookup("fma"))
      GTEST_SKIP() << "native OS-enabled FMA3 required";
    const uint8_t Arg = memoryModRM();
    const uint8_t Prefix = Double ? 0xf2 : 0xf3;
    std::vector<uint8_t> Bytes = {
        0xc5, 0xfc, 0x57,   0xc0, Prefix, 0x0f,
        0x10, Arg,  Prefix, 0x0f, 0x10,   static_cast<uint8_t>(0x48 | Arg),
        16};
    if (Double) {
      const uint64_t One = UINT64_C(0x3ff0000000000000);
      Bytes.insert(Bytes.end(), {0x48, 0xba});
      for (unsigned Index = 0; Index < 8; ++Index)
        Bytes.push_back(uint8_t(One >> (Index * 8)));
      Bytes.insert(Bytes.end(), {0x66, 0x48, 0x0f, 0x6e, 0xd2});
    } else {
      Bytes.insert(Bytes.end(),
                   {0xba, 0, 0, 0x80, 0x3f, 0x66, 0x0f, 0x6e, 0xd2});
    }
    const unsigned Opcode = (Order == 132   ? 0x98
                             : Order == 213 ? 0xa8
                                            : 0xb8) +
                            1 + (Negate ? 4 : 0);
    Bytes.insert(Bytes.end(),
                 {0xc4, 0xe2, static_cast<uint8_t>(Double ? 0xf1 : 0x71),
                  static_cast<uint8_t>(Opcode), 0xc2});
    if (Keep)
      Bytes.insert(Bytes.end(),
                   {Prefix, 0x0f, 0x11, static_cast<uint8_t>(0x40 | Arg), 32});
    Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
    compareFloating(Bytes, Double);
  }
};

TEST_F(X86FPFmaStateAccuracy, DiscardedScalarFmaRetainsMXCSR) {
  scalar(false, 213, false, false);
  scalar(true, 213, false, false);
}
TEST_F(X86FPFmaStateAccuracy, NegatedProductKeepsOriginalNaNSourceBits) {
  scalar(false, 213, true, true);
  scalar(true, 213, true, true);
}
TEST_F(X86FPFmaStateAccuracy, ScalarOperandOrdersMatchNativeRawBitsAndState) {
  for (unsigned Order : {132U, 213U, 231U}) {
    scalar(false, Order, false, true);
    scalar(true, Order, false, true);
  }
}
struct FmaForm {
  unsigned Operation, Order;
  bool Double, Scalar;
  unsigned Bytes;
  bool Memory;
};
std::vector<FmaForm> fmaForms() {
  std::vector<FmaForm> Forms;
  for (unsigned Operation = 0; Operation < 6; ++Operation)
    for (unsigned Order = 0; Order < 3; ++Order)
      for (bool Double : {false, true})
        for (bool Memory : {false, true})
          for (const auto [Scalar, Bytes] :
               {std::pair{true, 16U}, std::pair{false, 16U},
                std::pair{false, 32U}})
            if (!Scalar || Operation < 4)
              Forms.push_back(
                  {Operation, Order, Double, Scalar, Bytes, Memory});
  return Forms;
}
std::string fmaName(const FmaForm &Form) {
  const char *Names[] = {"Fmadd",  "Fmsub",    "Fnmadd",
                         "Fnmsub", "Fmaddsub", "Fmsubadd"};
  const unsigned Orders[] = {132, 213, 231};
  return std::string(Names[Form.Operation]) +
         std::to_string(Orders[Form.Order]) +
         (Form.Scalar ? (Form.Double ? "SD" : "SS")
                      : (Form.Double ? "PD" : "PS")) +
         (Form.Bytes == 32 ? "Vex256" : "Vex128") +
         (Form.Memory ? "Memory" : "Register");
}
class X86FPFmaVectorFixture : public X86FPStateFixture {
protected:
  bool KeepResult = true, AliasSource1 = false, AliasRHS = false;
  unsigned SourceOffset = 0;
  std::string comparisonDriver(bool Double, bool Declare, bool Fault,
                               bool NativeCall = false) override {
    auto Text = floatingDriver(Double, Declare);
    Text.resize(Text.find("int main(void)"));
    Text += "enum { source_offset=" + std::to_string(SourceOffset) + " };\n";
    Text += R"(
int main(void) {
  uint32_t saved=_mm_getcsr();
  for(unsigned rc=0;rc<4;++rc)
    for(unsigned environment=0;environment<4;++environment)
      for(unsigned sticky=0;sticky<2;++sticky)
        for(unsigned phase=0;phase<3;++phase)
          for(unsigned a=0;a<sizeof(values)/sizeof(values[0]);++a)
            for(unsigned b=0;b<sizeof(values)/sizeof(values[0]);++b) {
              _Alignas(64) unsigned char expected[160],actual[160];
              for(unsigned i=0;i<160;++i) expected[i]=(unsigned char)(0x59u+i*37u);
              for(unsigned lane=0;lane<32/sizeof(values[0]);++lane) {
                unsigned count=sizeof(values)/sizeof(values[0]);
                unsigned ai=(a+lane*3)%count,bi=(b+lane*7)%count;
                unsigned ci=(a*3+b*7+lane*11+phase*5)%count;
                memcpy(expected+lane*sizeof(values[0]),&values[ai],sizeof(values[0]));
                memcpy(expected+32+lane*sizeof(values[0]),&values[bi],sizeof(values[0]));
                memcpy(expected+64+source_offset+lane*sizeof(values[0]),&values[ci],sizeof(values[0]));
              }
              memcpy(actual,expected,160);
              uint32_t state=0x1f80|(rc<<13)|((environment&1)?0x40:0)|
                ((environment&2)?0x8000:0)|(sticky?0x25:0);
              _mm_setcsr(state);native_probe((uintptr_t)expected);
              uint32_t wanted=_mm_getcsr();
              _mm_setcsr(state);isa_probe((uintptr_t)actual);
              uint32_t got=_mm_getcsr();_mm_setcsr(saved);
              if(wanted!=got || memcmp(expected,actual,160)) {
                printf("a=%u b=%u phase=%u state=%08x expected=%08x actual=%08x\n",a,b,phase,state,wanted,got);
                for(unsigned i=0;i<160;++i)
                  if(expected[i]!=actual[i])printf("byte%u expected=%02x actual=%02x\n",i,expected[i],actual[i]);
                return 1;
              }
            }
  return 0;
}
)";
    return Text;
  }
  std::vector<uint8_t> bytes(const FmaForm &Form) {
    const unsigned Destination = AliasRHS && !Form.Memory ? 2 : 0;
    const unsigned Source1 = AliasSource1 ? Destination : 1;
    const auto Arg = memoryModRM();
    std::vector<uint8_t> Bytes = {
        0xc5, 0xfe, 0x6f, static_cast<uint8_t>((Destination << 3) | Arg)};
    if (!AliasSource1)
      Bytes.insert(Bytes.end(),
                   {0xc5, 0xfe, 0x6f, static_cast<uint8_t>(0x48 | Arg), 32});
    if (!Form.Memory && !AliasRHS)
      Bytes.insert(Bytes.end(),
                   {0xc5, 0xfe, 0x6f, static_cast<uint8_t>(0x50 | Arg), 64});
    const unsigned Opcodes[] = {0x98, 0x9a, 0x9c, 0x9e, 0x96, 0x97};
    Bytes.insert(Bytes.end(), {0xc4, 0xe2,
                               static_cast<uint8_t>((Form.Double ? 0x80 : 0) |
                                                    ((~Source1 & 15) << 3) | 1 |
                                                    (Form.Bytes == 32 ? 4 : 0)),
                               static_cast<uint8_t>(Opcodes[Form.Operation] +
                                                    Form.Order * 16 +
                                                    (Form.Scalar ? 1 : 0))});
    if (Form.Memory)
      Bytes.insert(Bytes.end(),
                   {static_cast<uint8_t>(0x40 | (Destination << 3) | Arg),
                    static_cast<uint8_t>(64 + SourceOffset)});
    else
      Bytes.push_back(static_cast<uint8_t>(0xc2 | (Destination << 3)));
    if (KeepResult)
      Bytes.insert(Bytes.end(),
                   {0xc5, 0xfe, 0x7f,
                    static_cast<uint8_t>(0x40 | (Destination << 3) | Arg), 96});
    Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
    return Bytes;
  }
  void check(const FmaForm &Form, bool Fault = false) {
    if (!llvm::sys::getHostCPUFeatures().lookup("fma"))
      GTEST_SKIP() << "native OS-enabled FMA3 required";
    SCOPED_TRACE(fmaName(Form));
    compareFloating(bytes(Form), Form.Double, Fault);
  }
};
class X86FPFmaForms : public X86FPFmaVectorFixture,
                      public testing::WithParamInterface<FmaForm> {};
TEST_P(X86FPFmaForms, NumericalAndMXCSRCompleteTogether) { check(GetParam()); }
INSTANTIATE_TEST_SUITE_P(FmaForms, X86FPFmaForms, testing::ValuesIn(fmaForms()),
                         [](const testing::TestParamInfo<FmaForm> &Info) {
                           return fmaName(Info.param);
                         });

TEST_F(X86FPFmaVectorFixture, DiscardedPackedResultsRetainCompleteState) {
  KeepResult = false;
  for (unsigned Operation : {0U, 2U, 4U, 5U})
    check({Operation, 1, false, false, 32, true});
}
TEST_F(X86FPFmaVectorFixture, ScalarAndPackedMemoryAllowUnalignedSources) {
  SourceOffset = 1;
  check({2, 0, false, true, 16, true});
  check({4, 2, true, false, 32, true});
}
TEST_F(X86FPFmaVectorFixture, PhysicalSourcesRetainDestinationAliases) {
  for (unsigned Order = 0; Order < 3; ++Order) {
    AliasSource1 = true;
    check({2, Order, false, true, 16, false});
    AliasSource1 = false;
    AliasRHS = true;
    check({4, Order, true, false, 32, false});
    AliasRHS = false;
  }
}

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
template <typename Scalar, unsigned Bytes>
__attribute__((target("avx,fma")))
std::pair<std::array<uint8_t, Bytes>, uint32_t>
nativeFmaState(unsigned Control, const uint8_t *A, const uint8_t *B,
               const uint8_t *C, uint32_t State) {
  typedef Scalar Packed __attribute__((vector_size(Bytes < 16 ? 16 : Bytes)));
  using Value = std::conditional_t<(Bytes < 16), Scalar, Packed>;
  Value Destination, Source1, Source2;
  std::memcpy(&Destination, A, Bytes);
  std::memcpy(&Source1, B, Bytes);
  std::memcpy(&Source2, C, Bytes);
  const uint32_t Saved = _mm_getcsr();
#define FMA_ASM(OP, SUFFIX)                                                    \
  __asm__ volatile("ldmxcsr %1\n\t" OP SUFFIX " %3,%2,%0\n\tstmxcsr %1"        \
                   : "+x"(Destination), "+m"(State)                            \
                   : "x"(Source1), "x"(Source2)                                \
                   : "memory")
#define RUN_FMA(OP)                                                            \
  if constexpr (Bytes < 16) {                                                  \
    if constexpr (sizeof(Scalar) == 8) {                                       \
      FMA_ASM(OP, "sd");                                                       \
    } else {                                                                   \
      FMA_ASM(OP, "ss");                                                       \
    }                                                                          \
  } else {                                                                     \
    if constexpr (sizeof(Scalar) == 8) {                                       \
      FMA_ASM(OP, "pd");                                                       \
    } else {                                                                   \
      FMA_ASM(OP, "ps");                                                       \
    }                                                                          \
  }
#define FMA_CASE(ID, OP)                                                       \
  case ID: {                                                                   \
    RUN_FMA(OP);                                                               \
    break;                                                                     \
  }
#define FMA_ALT_CASE(ID, OP)                                                   \
  case ID: {                                                                   \
    if constexpr (Bytes >= 16) {                                               \
      RUN_FMA(OP);                                                             \
    } else {                                                                   \
      std::abort();                                                            \
    }                                                                          \
    break;                                                                     \
  }
  const unsigned Operation =
      (Control & 1024) ? ((Control & 2048) ? 4 : 5)
                       : ((Control & 64) ? 2 : 0) + ((Control & 128) ? 1 : 0);
  switch (((Control >> 8) & 3) * 6 + Operation) {
    FMA_CASE(0, "vfmadd132");
    FMA_CASE(1, "vfmsub132");
    FMA_CASE(2, "vfnmadd132");
    FMA_CASE(3, "vfnmsub132");
    FMA_ALT_CASE(4, "vfmaddsub132");
    FMA_ALT_CASE(5, "vfmsubadd132");
    FMA_CASE(6, "vfmadd213");
    FMA_CASE(7, "vfmsub213");
    FMA_CASE(8, "vfnmadd213");
    FMA_CASE(9, "vfnmsub213");
    FMA_ALT_CASE(10, "vfmaddsub213");
    FMA_ALT_CASE(11, "vfmsubadd213");
    FMA_CASE(12, "vfmadd231");
    FMA_CASE(13, "vfmsub231");
    FMA_CASE(14, "vfnmadd231");
    FMA_CASE(15, "vfnmsub231");
    FMA_ALT_CASE(16, "vfmaddsub231");
    FMA_ALT_CASE(17, "vfmsubadd231");
  default:
    std::abort();
  }
#undef FMA_ALT_CASE
#undef FMA_CASE
#undef RUN_FMA
#undef FMA_ASM
  std::array<uint8_t, Bytes> Result;
  std::memcpy(Result.data(), &Destination, Bytes);
  _mm_setcsr(Saved);
  return {Result, State};
}
#endif

LowOp fmaStateOp(const FmaForm &Form, uint32_t State) {
  const unsigned Bytes = Form.Scalar ? (Form.Double ? 8 : 4) : Form.Bytes;
  const unsigned Control = makeX86FPFmaStateControl(
      Form.Order, Form.Double, Form.Scalar, Form.Memory,
      Form.Operation < 4 && (Form.Operation & 2),
      Form.Operation < 4 && (Form.Operation & 1), Form.Operation >= 4,
      Form.Operation == 4);
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::reg(10000, Bytes + 4);
  Op.addInput(NdVar::cst(unsigned(Form.Memory ? Intrinsic::X86FPFmaMemoryState
                                              : Intrinsic::X86FPFmaState),
                         2));
  if (Form.Memory) {
    Op.addInput(NdVar::cst(0x2000, 8));
    Op.addInput(NdVar::cst(Control, 2));
    Op.addInput(NdVar::reg(10001, Bytes));
    Op.addInput(NdVar::reg(10002, Bytes));
  } else {
    Op.addInput(NdVar::cst(Control, 2));
    Op.addInput(NdVar::reg(10001, Bytes));
    Op.addInput(NdVar::reg(10002, Bytes));
    Op.addInput(NdVar::reg(10003, Bytes));
  }
  Op.addInput(NdVar::cst(State, 4));
  return Op;
}

TEST(X86FPFmaContract, ConcreteStateMatchesNativeRawBitsAndMXCSR) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  if (!llvm::sys::getHostCPUFeatures().lookup("fma"))
    GTEST_SKIP() << "native FMA3 required";
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
  uint64_t Comparisons = 0;
  for (const auto &Form : fmaForms()) {
    const auto &Values = Form.Double ? Doubles : Singles;
    const unsigned Element = Form.Double ? 8 : 4,
                   Bytes = Form.Scalar ? Element : Form.Bytes;
    for (unsigned RC = 0; RC < 4; ++RC)
      for (unsigned Environment = 0; Environment < 4; ++Environment)
        for (unsigned Sticky = 0; Sticky < 2; ++Sticky)
          for (unsigned A = 0; A < 18; ++A)
            for (unsigned B = 0; B < 18; ++B)
              for (unsigned Phase = 0; Phase < 3; ++Phase) {
                const uint32_t State =
                    0x1f80 | (RC << 13) | ((Environment & 1) ? 0x40 : 0) |
                    ((Environment & 2) ? 0x8000 : 0) | (Sticky ? 0x25 : 0);
                const auto Op = fmaStateOp(Form, State);
                const unsigned Control = Op.Inputs[Form.Memory ? 2 : 1].Offset;
                std::vector<uint8_t> Dest(Bytes), Src1(Bytes), Src2(Bytes);
                for (unsigned Lane = 0; Lane < Bytes / Element; ++Lane) {
                  auto D = Values[(A + Lane * 3) % 18],
                       S1 = Values[(B + Lane * 7) % 18],
                       S2 =
                           Values[(A * 3 + B * 7 + Lane * 11 + Phase * 5) % 18];
                  std::memcpy(Dest.data() + Lane * Element, &D, Element);
                  std::memcpy(Src1.data() + Lane * Element, &S1, Element);
                  std::memcpy(Src2.data() + Lane * Element, &S2, Element);
                }
                std::vector<uint8_t> Expected(Bytes + 4);
                uint32_t Outgoing = 0;
                const auto Copy = [&](const auto &Result) {
                  std::memcpy(Expected.data(), Result.first.data(), Bytes);
                  Outgoing = Result.second;
                };
                if (Bytes == 4)
                  Copy(nativeFmaState<float, 4>(
                      Control, Dest.data(), Src1.data(), Src2.data(), State));
                else if (Bytes == 8)
                  Copy(nativeFmaState<double, 8>(
                      Control, Dest.data(), Src1.data(), Src2.data(), State));
                else if (Bytes == 16 && Form.Double)
                  Copy(nativeFmaState<double, 16>(
                      Control, Dest.data(), Src1.data(), Src2.data(), State));
                else if (Bytes == 16)
                  Copy(nativeFmaState<float, 16>(
                      Control, Dest.data(), Src1.data(), Src2.data(), State));
                else if (Form.Double)
                  Copy(nativeFmaState<double, 32>(
                      Control, Dest.data(), Src1.data(), Src2.data(), State));
                else
                  Copy(nativeFmaState<float, 32>(
                      Control, Dest.data(), Src1.data(), Src2.data(), State));
                std::memcpy(Expected.data() + Bytes, &Outgoing, 4);
                BinaryImage Image;
                Image.Arch = Arch::X64;
                Segment Source;
                Source.VA = 0x2000;
                Source.Size = Source.FileSz = Bytes;
                Source.Flags = SegmentFlags::Readable;
                Source.Data = Src2;
                Image.Segments.push_back(Source);
                NdOpEmulator Emulator(Image);
                Emulator.setStrictMode(true);
                Emulator.setX86LinearAddressBits(48);
                Emulator.setRegisterBytes(10001, Dest);
                Emulator.setRegisterBytes(10002, Src1);
                Emulator.setRegisterBytes(10003, Src2);
                const bool Complete = Emulator.step(Op);
                if (!Complete || Emulator.getRegisterBytes(10000) != Expected ||
                    Emulator.getMXCSR() != Outgoing) {
                  ADD_FAILURE() << fmaName(Form) << " a=" << A << " b=" << B
                                << " phase=" << Phase << " incoming=" << State
                                << " expected state=" << Outgoing
                                << " actual state=" << Emulator.getMXCSR();
                  return;
                }
                ++Comparisons;
              }
  }
  EXPECT_EQ(Comparisons, UINT64_C(5971968));
#else
  GTEST_SKIP() << "native x64 FMA oracle requires GCC/Clang";
#endif
}

class X86FPFmaFaultAccuracy : public X86FPFmaVectorFixture {
protected:
  bool ProtectedSource = false;
  unsigned Unmask = 1, ExpectedFlags = 3, SourceBytes = 16;
  std::string comparisonDriver(bool Double, bool Declare, bool Fault,
                               bool NativeCall = false) override {
    if (!Fault)
      return X86FPFmaVectorFixture::comparisonDriver(Double, Declare, Fault,
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
    const unsigned Incoming = 0x1f80 & ~(Unmask << 7);
    Text += "enum { incoming=" + std::to_string(Incoming) +
            ", expected_csr=" + std::to_string(Incoming | ExpectedFlags) +
            ", protected_source=" + std::to_string(ProtectedSource) + " };\n";
    Text += R"(
static int matches(uint32_t csr,uintptr_t address,int memory_fault) {
  if(csr!=expected_csr || memory_fault!=protected_source)return 0;
  if(protected_source)return address>=source_first && address<source_last;
  for(unsigned i=96;i<128;++i)if(actual[i]!=(unsigned char)(0x59u+i*37u))return 0;
  return 1;
}
#if defined(_WIN32)
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *info) {
  DWORD code=info->ExceptionRecord->ExceptionCode;
  int memory=code==EXCEPTION_ACCESS_VIOLATION;
  if(memory || code==EXCEPTION_FLT_INVALID_OPERATION || code==EXCEPTION_FLT_DENORMAL_OPERAND ||
     code==EXCEPTION_FLT_OVERFLOW || code==EXCEPTION_FLT_UNDERFLOW || code==EXCEPTION_FLT_INEXACT_RESULT ||
     code==STATUS_FLOAT_MULTIPLE_FAULTS || code==STATUS_FLOAT_MULTIPLE_TRAPS) {
    uintptr_t address=memory&&info->ExceptionRecord->NumberParameters>1?
      (uintptr_t)info->ExceptionRecord->ExceptionInformation[1]:0;
    ExitProcess(matches(info->ContextRecord->MxCsr,address,memory)?0:1);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void on_fault(int number,siginfo_t *info,void *opaque) {
  ucontext_t *context=(ucontext_t *)opaque;
  _exit(matches(context->uc_mcontext.fpregs->mxcsr,(uintptr_t)info->si_addr,
                number==SIGSEGV || number==SIGBUS)?0:1);
}
#endif
int main(void) {
#if defined(_WIN32)
  if(!AddVectoredExceptionHandler(1,on_fault))return 77;
#else
  struct sigaction action={0};action.sa_sigaction=on_fault;action.sa_flags=SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if(sigaction(SIGFPE,&action,0)||sigaction(SIGSEGV,&action,0)||sigaction(SIGBUS,&action,0))return 77;
#endif
  if(protected_source) {
#if defined(_WIN32)
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
    unsigned char *arena=VirtualAlloc(0,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    DWORD old;if(!arena||!VirtualProtect(arena+page,page,PAGE_NOACCESS,&old))return 77;
#else
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *arena=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(arena==MAP_FAILED||mprotect(arena+page,page,PROT_NONE))return 77;
#endif
    memset(arena,0,page);actual=arena+page-68;source_first=(uintptr_t)(actual+64);
)";
    Text +=
        "    source_last=source_first+" + std::to_string(SourceBytes) + ";\n";
    Text += R"(
  } else {
    for(unsigned i=0;i<160;++i)actual[i]=(unsigned char)(0x59u+i*37u);
    const uint32_t a[8]={0,1,0x7f7fffff,0x3f800000,0,1,0x7f7fffff,0x3f800000};
    const uint32_t b[8]={0x7f800000,0x3f800000,0x7f7fffff,0x40000000,
                        0x7f800000,0x3f800000,0x7f7fffff,0x40000000};
    const uint32_t c[8]={0x3f800000,0x3f800000,0x3f800000,0x3f800000,
                        0x3f800000,0x3f800000,0x3f800000,0x3f800000};
    memcpy(actual,a,32);memcpy(actual+32,b,32);memcpy(actual+64,c,32);
  }
  _mm_setcsr(incoming);
)";
    Text += std::string("  ") + (NativeCall ? "native_probe" : "isa_probe") +
            "((uintptr_t)actual);\n";
    Text += "  _mm_setcsr(0x1f80);return 99;\n}\n";
    return Text;
  }
  void runFault(bool Memory, unsigned Mask, unsigned Flags) {
    Unmask = Mask;
    ExpectedFlags = Flags;
    check({0, 1, false, false, 32, Memory}, true);
  }
};
TEST_F(X86FPFmaFaultAccuracy, InvalidPrecedesOtherLanePrecisionWithoutWrites) {
  for (bool Memory : {false, true})
    runFault(Memory, 1, 3);
}
TEST_F(X86FPFmaFaultAccuracy, OverflowRetainsCompletedExceptionEvidence) {
  for (bool Memory : {false, true})
    runFault(Memory, 8, 0x2b);
}
TEST_F(X86FPFmaFaultAccuracy, DiscardedResultStillRaisesUnmaskedInvalid) {
  KeepResult = false;
  runFault(true, 1, 3);
}
TEST_F(X86FPFmaFaultAccuracy,
       DiscardedVectorRequiresTheCompleteProtectedSource) {
  KeepResult = false;
  ProtectedSource = true;
  SourceBytes = 32;
  runFault(true, 0, 0);
}

TEST(X86FPFmaContract, MalformedShapesAndControlsRefuseWithoutWrites) {
  for (bool Memory : {false, true})
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      BinaryImage Image;
      Image.Arch = Arch::X64;
      auto Op = fmaStateOp({0, 1, false, false, 16, Memory}, 0x1f80);
      if (Mutation == 0)
        Op.NumInputs = 5;
      if (Mutation == 1)
        Op.Inputs[Memory ? 2 : 1].Size = 1;
      if (Mutation == 2)
        Op.Inputs[Memory ? 2 : 1].Offset |= 4096;
      if (Mutation == 3)
        Op.Inputs[Memory ? 2 : 1].Offset |= 3 << 8;
      if (Mutation == 4)
        Op.Inputs[Memory ? 2 : 1].Offset |= 2048;
      if (Mutation == 5)
        Op.Inputs[4].Size = 8;
      if (Mutation == 6)
        Op.Inputs[5].Size = 8;
      if (Mutation == 7)
        Op.Output.Size = 16;
      const auto Id =
          Memory ? Intrinsic::X86FPFmaMemoryState : Intrinsic::X86FPFmaState;
      EXPECT_FALSE(
          x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X64)));
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setMXCSR(0x1fa5);
      const std::vector<uint8_t> Sentinel(Op.Output.Size, 0x59);
      Emulator.setRegisterBytes(10000, Sentinel);
      EXPECT_FALSE(Emulator.step(Op));
      EXPECT_EQ(Emulator.getRegisterBytes(10000), Sentinel);
      EXPECT_EQ(Emulator.getMXCSR(), 0x1fa5U);
    }
}

TEST(X86FPFmaContract, OwnedAssemblyRequiresAllThreeSourcesAndAStatePointer) {
  for (bool Memory : {false, true})
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      llvm::LLVMContext Context;
      llvm::Module Module("fma-owned", Context);
      llvm::IRBuilder<> Builder(Context);
      const unsigned Control =
          makeX86FPFmaStateControl(1, true, true, Memory, true, true);
      const auto Space =
          Memory ? NdMemoryAddressSpace::X86GS : NdMemoryAddressSpace::Default;
      const unsigned Layout = x86FPRoundStateLayout(8, Control, 0, Space);
      auto *Type = x86FPArithStateLLVMType(Context, Layout);
      llvm::Type *RHS = Memory ? static_cast<llvm::Type *>(Builder.getPtrTy(
                                     llvmX86MemoryAddressSpace(Space)))
                               : Type;
      llvm::Type *State = Builder.getPtrTy();
      if (Mutation == 1)
        RHS = Builder.getInt64Ty();
      if (Mutation == 2)
        State = Builder.getPtrTy(
            llvmX86MemoryAddressSpace(NdMemoryAddressSpace::X86GS));
      std::vector<llvm::Type *> Inputs = {Type, Type, RHS, State};
      if (Mutation == 3)
        Inputs.erase(Inputs.begin() + 1);
      auto *Signature = llvm::FunctionType::get(Type, Inputs, false);
      std::string Text = x86FPArithStateAsm(Layout, Memory);
      if (Mutation == 4)
        Text += "\n\tnop";
      std::string Constraints = x86FPArithStateConstraints(Layout, Memory);
      if (Mutation == 5)
        Constraints = Memory ? "=x,0,x,r,r,~{memory}" : "=x,0,x,x,r,~{memory}";
      auto *Asm =
          llvm::InlineAsm::get(Signature, Text, Constraints, Mutation != 6);
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(Builder.getVoidTy(), false),
          llvm::GlobalValue::ExternalLinkage, "probe", Module);
      Builder.SetInsertPoint(
          llvm::BasicBlock::Create(Context, "entry", Function));
      std::vector<llvm::Value *> Args;
      for (auto *Input : Inputs)
        Args.push_back(llvm::UndefValue::get(Input));
      auto *Call = Builder.CreateCall(Asm, Args);
      if (Mutation != 7)
        Call->setMetadata(X86FPStateAsmMetadata,
                          llvm::MDNode::get(Context, {}));
      Builder.CreateRetVoid();
      if (Mutation == 0) {
        const auto Shape = classifyX86FPStateAsm(*Call);
        ASSERT_TRUE(Shape);
        EXPECT_EQ(Shape->first, Memory ? Intrinsic::X86FPFmaMemoryState
                                       : Intrinsic::X86FPFmaState);
        EXPECT_EQ(Shape->second, Layout);
      } else if (Mutation == 7)
        EXPECT_FALSE(classifyX86FPStateAsm(*Call));
      else
        EXPECT_DEATH(classifyX86FPStateAsm(*Call),
                     "invalid x86 FP state assembly");
    }
}

} // namespace
