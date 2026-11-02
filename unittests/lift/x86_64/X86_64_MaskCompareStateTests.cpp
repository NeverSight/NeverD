//===- X86_64_MaskCompareStateTests.cpp - CMP mask/MXCSR accuracy --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original SSE/VEX comparison bytes and all predicate/state observations.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/ir/high/X86/X86FPStateShape.h"

namespace {
struct MaskCompareForm {
  bool Double, Scalar, Vex, Memory;
  unsigned Width;
};
std::vector<MaskCompareForm> maskCompareForms() {
  std::vector<MaskCompareForm> Forms;
  for (bool Double : {false, true})
    for (bool Scalar : {false, true})
      for (bool Vex : {false, true})
        for (unsigned Width : {16U, 32U})
          for (bool Memory : {false, true})
            if (Width == 16 || (Vex && !Scalar))
              Forms.push_back({Double, Scalar, Vex, Memory, Width});
  return Forms;
}
std::string maskCompareName(const MaskCompareForm &F) {
  return std::string(F.Vex ? "Vex" : "Legacy") +
         (F.Scalar ? (F.Double ? "SD" : "SS") : (F.Double ? "PD" : "PS")) +
         std::to_string(F.Width * 8) + (F.Memory ? "Memory" : "Register");
}
class MaskCompareFixture : public X86FPStateFixture {
protected:
  MaskCompareForm Form{true, true, true, false, 16};
  std::vector<unsigned> Predicates{8};
  bool PointerProbe = false, PageProbe = false, GSProbe = false;
  bool DiscardMasks = false, MixedMemory = false;
  unsigned FaultMode = 0;
  void allPredicates() {
    Predicates.clear();
    for (unsigned P = 0; P < (Form.Vex ? 32u : 8u); ++P)
      Predicates.push_back(P);
  }
  std::vector<uint8_t> maskBytes() {
#if defined(_WIN32)
    const uint8_t Base = 1;
#else
    const uint8_t Base = 7;
#endif
    // XOR defines RAX but leaves AF undefined; CMP also defines observed AF.
    std::vector<uint8_t> Code{0x31, 0xc0, 0x39, 0xc0};
    const auto Add = [&](std::initializer_list<uint8_t> V) {
      Code.insert(Code.end(), V.begin(), V.end());
    };
    const auto D32 = [&](unsigned V) {
      for (unsigned I = 0; I < 4; ++I)
        Code.push_back(uint8_t(V >> (8 * I)));
    };
    if (PointerProbe)
      Add({0x4c, 0x8b, uint8_t(0x58 | Base), 72});
    Add({0xc5, 0xfc, 0x10, uint8_t(0x48 | Base), 32}); // Full right YMM1.
    unsigned Index = 0;
    for (unsigned Predicate : Predicates) {
      Add({0xc5, 0xfc, 0x10, Base}); // Reset full left YMM0 for legacy forms.
      Add({0x0f, 0xae, uint8_t(0x50 | Base), 64});
      unsigned PP = Form.Scalar ? (Form.Double ? 3 : 2) : (Form.Double ? 1 : 0);
      const bool Memory = Form.Memory || (MixedMemory && (Index & 1));
      if (GSProbe)
        Code.push_back(0x65);
      if (Form.Vex) {
        if (PointerProbe)
          Add({0xc4, 0xc1, uint8_t(0xf8 | PP | (Form.Width == 32 ? 4 : 0))});
        else
          Add({0xc5, uint8_t(0xf8 | PP | (Form.Width == 32 ? 4 : 0))});
      } else {
        if (PP)
          Code.push_back(PP == 1 ? 0x66 : PP == 2 ? 0xf3 : 0xf2);
        if (PointerProbe)
          Code.push_back(0x41);
        Code.push_back(0x0f);
      }
      Code.push_back(0xc2);
      const unsigned Destination = Form.Vex ? 2 : 0;
      if (PointerProbe)
        Code.push_back(uint8_t((Destination << 3) | 3));
      else if (Memory)
        Add({uint8_t(0x40 | (Destination << 3) | Base), 32});
      else
        Code.push_back(uint8_t(0xc1 | (Destination << 3)));
      Code.push_back(uint8_t(Predicate));
      if (!DiscardMasks) {
        Add({0xc5, 0xfc, 0x11, uint8_t(0x80 | (Destination << 3) | Base)});
        D32(128 + Index * 32);
      }
      Add({0x0f, 0xae, uint8_t(0x98 | Base)});
      D32(1184 + Index * 4);
      ++Index;
    }
    Add({0x9f, 0x88, uint8_t(0xa0 | Base)}); // LAHF includes AF.
    D32(1312);
    Add({0x0f, 0x90, uint8_t(0x80 | Base)});
    D32(1313);
    Add({0xc5, 0xfc, 0x11, uint8_t(0x88 | Base)});
    D32(1360);
    if (!DiscardMasks) {
      Add({0xc5, 0xfc, 0x11, uint8_t(0x80 | Base)});
      D32(1392);
    }
    Code.push_back(0xc3);
    return Code;
  }
  static std::string pageAllocation() {
    return R"(
#if defined(_WIN32)
#include <windows.h>
static unsigned char *pages;
static size_t page_size;
static int allocate_pages(void) {
  SYSTEM_INFO info;GetSystemInfo(&info);page_size=info.dwPageSize;
  pages=VirtualAlloc(0,page_size*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
  DWORD old;
  return pages&&VirtualProtect(pages+page_size,page_size,PAGE_NOACCESS,&old);
}
#else
#include <sys/mman.h>
#include <unistd.h>
static unsigned char *pages;
static size_t page_size;
static int allocate_pages(void) {
  page_size=sysconf(_SC_PAGESIZE);
  pages=mmap(0,page_size*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  return pages!=MAP_FAILED&&!mprotect(pages+page_size,page_size,PROT_NONE);
}
#endif
)";
  }
  std::string maskFaultDriver(bool Declare, bool Native) {
    const bool MemoryFault = FaultMode >= 4;
    const unsigned Incoming = MemoryFault      ? 0x1f80
                              : FaultMode == 1 ? 0x1f00
                              : FaultMode == 2 ? 0x1e80
                                               : 0x1e00;
    const unsigned Expected = Incoming | (MemoryFault      ? 0
                                          : FaultMode == 1 ? 1
                                          : FaultMode == 2 ? 2
                                                           : 3);
    std::string Text =
        "#define _GNU_SOURCE\n#include <stdint.h>\n#include "
        "<stdio.h>\n#include <string.h>\n#include <stdlib.h>\n#include "
        "<immintrin.h>\nextern void native_probe(uintptr_t);\n";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += "#define EXPECTED_CSR " + std::to_string(Expected) +
            "u\n#define MEMORY_FAULT " + std::to_string(MemoryFault) +
            "\n#define ALIGNMENT_FAULT " + std::to_string(FaultMode == 5) +
            "\n";
    if (MemoryFault)
      Text += pageAllocation();
    Text += R"(
static unsigned char actual[1536] __attribute__((aligned(32))),before[1536];
static int valid(unsigned csr) {
  if(csr!=EXPECTED_CSR) {printf("CSR expected=%08x actual=%08x\n",EXPECTED_CSR,csr);return 0;}
  return !memcmp(actual,before,sizeof(actual));
}
#if defined(_WIN32)
#include <windows.h>
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *p) {
  if(p->ExceptionRecord->ExceptionCode!=(MEMORY_FAULT?EXCEPTION_ACCESS_VIOLATION:EXCEPTION_FLT_INVALID_OPERATION))
    return EXCEPTION_CONTINUE_SEARCH;
#if MEMORY_FAULT && !ALIGNMENT_FAULT
  if(p->ExceptionRecord->NumberParameters!=2 || p->ExceptionRecord->ExceptionInformation[0]!=0 ||
     p->ExceptionRecord->ExceptionInformation[1]<(uintptr_t)(pages+page_size) ||
     p->ExceptionRecord->ExceptionInformation[1]>=(uintptr_t)(pages+page_size+32))ExitProcess(2);
#endif
  ExitProcess(valid(p->ContextRecord->MxCsr)?0:1);
}
#elif defined(__linux__)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
static void on_fault(int sig,siginfo_t *p,void *context) {
  ucontext_t *u=(ucontext_t*)context;
  _exit(sig==(MEMORY_FAULT?SIGSEGV:SIGFPE)&&valid(u->uc_mcontext.fpregs->mxcsr)?0:1);
}
#endif
int main(void) {
#if defined(_WIN32)
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  if(!AddVectoredExceptionHandler(1,on_fault))return 77;
#else
  struct sigaction action={0};action.sa_sigaction=on_fault;action.sa_flags=SA_SIGINFO;
  sigemptyset(&action.sa_mask);if(sigaction(MEMORY_FAULT?SIGSEGV:SIGFPE,&action,0))return 77;
#endif
  memset(actual,0xa5,sizeof(actual));memset(actual,0,64);
)";
    const uint64_t NaN =
        Form.Double ? UINT64_C(0x7ff0000000000071) : 0x7f800071;
    const uint64_t Left = FaultMode == 2 ? 1 : NaN;
    Text += "  const uint64_t left=" + std::to_string(Left) +
            "ULL; memcpy(actual,&left," + std::to_string(Form.Double ? 8 : 4) +
            ");\n";
    if (FaultMode == 3)
      Text += "  actual[" + std::to_string(Form.Width - (Form.Double ? 8 : 4)) +
              "]=1;\n";
    Text += "  const uint32_t state=" + std::to_string(Incoming) +
            "u; memcpy(actual+64,&state,4);\n";
    if (MemoryFault) {
      Text += "  if(!allocate_pages())return 77; uintptr_t "
              "address=(uintptr_t)(pages";
      Text += FaultMode == 5                ? "+1"
              : (!Form.Vex && !Form.Scalar) ? "+page_size"
                                            : "+page_size-1";
      Text += "); memcpy(actual+72,&address,sizeof(address));\n";
    }
    Text += "  memcpy(before,actual,sizeof(actual));\n";
    Text += Native ? "  native_probe((uintptr_t)actual);\n"
                   : "  isa_probe((uintptr_t)actual);\n";
    Text += "  _mm_setcsr(0x1f80);return 99; /* missing expected fault */\n}\n";
    return Text;
  }
  std::string comparisonDriver(bool Double, bool Declare, bool Fault,
                               bool Native = false) override {
    if (Fault)
      return maskFaultDriver(Declare, Native);
    std::string Text = R"(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <immintrin.h>
extern void native_probe(uintptr_t);
)";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += Double ? R"(
static const uint64_t values[] = {0,0x8000000000000000ULL,1,
  0x8000000000000001ULL,0x000fffffffffffffULL,0x0010000000000000ULL,
  0x3ff0000000000000ULL,0xbff0000000000000ULL,0x7ff0000000000000ULL,
  0xfff0000000000000ULL,0x7ff8000000000031ULL,0x7ff0000000000071ULL};
)"
                   : R"(
static const uint32_t values[] = {0,0x80000000,1,0x80000001,0x007fffff,
  0x00800000,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc00031,
  0x7f800071};
)";
    if (PageProbe)
      Text += pageAllocation();
    if (GSProbe)
      Text += "\n#include <intrin.h>\n";
    Text += "#define ACTIVE_LANES " +
            std::to_string(Form.Scalar ? 1 : Form.Width / (Double ? 8 : 4)) +
            "\n#define PREDICATES " + std::to_string(Predicates.size()) + "\n";
    Text += R"(
int main(void) {
  unsigned saved=_mm_getcsr(), count=0;
  /* page probe setup */
  for(unsigned env=0;env<32;++env)
    for(unsigned a=0;a<12;++a)
      for(unsigned b=0;b<12;++b) {
        unsigned char expected[1536] __attribute__((aligned(32)));
        unsigned char actual[1536] __attribute__((aligned(32)));
        memset(expected,0xa5,sizeof(expected));
        for(unsigned lane=0;lane<ACTIVE_LANES;++lane) {
          memcpy(expected+lane*sizeof(values[0]),&values[(a+lane)%12],sizeof(values[0]));
          memcpy(expected+32+lane*sizeof(values[0]),&values[(b+lane*3)%12],sizeof(values[0]));
        }
        unsigned state=0x1f80|((env&3)<<13)|((env&4)?0x40:0)|
                       ((env&8)?0x8000:0)|((env&16)?0x25:0);
        memcpy(expected+64,&state,4);
        memcpy(actual,expected,sizeof(actual));
        native_probe((uintptr_t)expected);_mm_setcsr(saved);
        isa_probe((uintptr_t)actual);_mm_setcsr(saved);
        if(memcmp(actual,expected,sizeof(actual))) {
          printf("env=%u a=%u b=%u\n",env,a,b);
          for(unsigned i=128;i<1424;++i)
            if(actual[i]!=expected[i])
              printf("offset=%u expected=%02x actual=%02x\n",i,expected[i],actual[i]);
          return 1;
        }
        count+=PREDICATES;
      }
  printf("%u exact mask/MXCSR instruction comparisons\n",count);
  return 0;
}
)";
    if (PageProbe) {
      const std::string Start = "  /* page probe setup */";
      Text.replace(Text.find(Start), Start.size(),
                   "  if(!allocate_pages())return 77;");
      const std::string Marker =
          "        memcpy(actual,expected,sizeof(actual));";
      const unsigned Bytes = Form.Scalar ? (Form.Double ? 8 : 4) : Form.Width;
      std::string Setup =
          "        uintptr_t address=(uintptr_t)(pages+page_size-" +
          std::to_string(Bytes) + ");\n";
      Setup += "        memcpy((void*)address,expected+32," +
               std::to_string(Bytes) + ");\n";
      if (GSProbe)
        Setup += "        address-=__readgsqword(0x30);\n";
      Setup +=
          "        memcpy(expected+72,&address,sizeof(address));\n" + Marker;
      Text.replace(Text.find(Marker), Marker.size(), Setup);
    }
    return Text;
  }
};
TEST_F(MaskCompareFixture, VexScalarPredicate8RetainsUnorderedEquality) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX required";
  compareFloating(maskBytes(), Form.Double);
}
class MaskCompareForms : public MaskCompareFixture,
                         public testing::WithParamInterface<MaskCompareForm> {};
TEST_P(MaskCompareForms, EveryPredicateAndMxcsrMatchesOriginalBytes) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX required";
  Form = GetParam();
  Predicates.clear();
  for (unsigned I = 0; I < (Form.Vex ? 32 : 8); ++I)
    Predicates.push_back(I);
  compareFloating(maskBytes(), Form.Double);
}

TEST_F(MaskCompareFixture, IgnoredImmediateBitsRetainTheWholeCompletion) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  Predicates = {0x20, 0x40, 0x80, 0xff, 0xe8};
  for (const auto &F : {MaskCompareForm{false, true, false, false, 16},
                        MaskCompareForm{true, false, true, false, 32}}) {
    Form = F;
    compareFloating(maskBytes(), Form.Double);
  }
}
TEST_F(MaskCompareFixture, DiscardedMasksRetainMxcsrAndOriginalFlags) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  DiscardMasks = true;
  Predicates = {1};
  for (const auto &F : {MaskCompareForm{false, false, false, true, 16},
                        MaskCompareForm{true, true, true, false, 16}}) {
    Form = F;
    compareFloating(maskBytes(), Form.Double);
  }
}
TEST_F(MaskCompareFixture,
       RegisterAndMemoryHelpersSharePredicatesWithoutNameCollisions) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  MixedMemory = true;
  Predicates = {8, 8, 8, 8};
  compareFloating(maskBytes(), true);
}
TEST_P(MaskCompareForms, UnmaskedInvalidRetainsExceptionStatusWithoutOutput) {
#if !defined(_WIN32) && !defined(__linux__)
  GTEST_SKIP() << "native fault context requires Windows/Linux";
#else
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  Form = GetParam();
  FaultMode = 1;
  Predicates = {0};
  compareFloating(maskBytes(), Form.Double, true);
#endif
}
TEST_P(MaskCompareForms, UnmaskedDenormalRetainsExceptionStatusWithoutOutput) {
#if !defined(_WIN32) && !defined(__linux__)
  GTEST_SKIP() << "native fault context requires Windows/Linux";
#else
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  Form = GetParam();
  FaultMode = 2;
  Predicates = {0};
  compareFloating(maskBytes(), Form.Double, true);
#endif
}
std::vector<MaskCompareForm> maskMemoryForms() {
  auto Forms = maskCompareForms();
  std::erase_if(Forms, [](const auto &F) { return !F.Memory; });
  return Forms;
}
std::vector<MaskCompareForm> maskPackedForms() {
  auto Forms = maskCompareForms();
  std::erase_if(Forms, [](const auto &F) { return F.Scalar; });
  return Forms;
}
std::vector<MaskCompareForm> maskAlignedForms() {
  auto Forms = maskCompareForms();
  std::erase_if(Forms,
                [](const auto &F) { return !F.Memory || F.Scalar || F.Vex; });
  return Forms;
}
class MaskMemoryForms : public MaskCompareFixture,
                        public testing::WithParamInterface<MaskCompareForm> {};
TEST_P(MaskMemoryForms, ExactSourceExtentAtProtectedPageEnd) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  Form = GetParam();
  PointerProbe = PageProbe = true;
  allPredicates();
  compareFloating(maskBytes(), Form.Double);
}
TEST_P(MaskMemoryForms,
       PartialOrUnreadableSourceFaultPrecedesFloatingCompletion) {
#if !defined(_WIN32) && !defined(__linux__)
  GTEST_SKIP() << "native fault context requires Windows/Linux";
#else
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  Form = GetParam();
  PointerProbe = true;
  FaultMode = 4;
  Predicates = {0};
  compareFloating(maskBytes(), Form.Double, true);
#endif
}
TEST_P(MaskMemoryForms, WindowsGsUsesActualBaseForPrivatePageEndSource) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << "native GS oracle requires Windows x64";
#else
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  Form = GetParam();
  PointerProbe = PageProbe = GSProbe = true;
  allPredicates();
  compareFloating(maskBytes(), Form.Double);
#endif
}
class MaskPackedForms : public MaskCompareFixture,
                        public testing::WithParamInterface<MaskCompareForm> {};
TEST_P(MaskPackedForms, MixedLaneInvalidAndDenormalRetainBothStatusesOnFault) {
#if !defined(_WIN32) && !defined(__linux__)
  GTEST_SKIP() << "native fault context requires Windows/Linux";
#else
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  Form = GetParam();
  FaultMode = 3;
  Predicates = {0};
  compareFloating(maskBytes(), Form.Double, true);
#endif
}
class MaskAlignedForms : public MaskCompareFixture,
                         public testing::WithParamInterface<MaskCompareForm> {};
TEST_P(MaskAlignedForms, LegacyPackedAlignmentFaultPrecedesInvalidAndOutput) {
#if !defined(_WIN32) && !defined(__linux__)
  GTEST_SKIP() << "native fault context requires Windows/Linux";
#else
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  Form = GetParam();
  PointerProbe = true;
  FaultMode = 5;
  Predicates = {0};
  compareFloating(maskBytes(), Form.Double, true);
#endif
}
INSTANTIATE_TEST_SUITE_P(MemoryForms, MaskMemoryForms,
                         testing::ValuesIn(maskMemoryForms()),
                         [](const auto &I) {
                           return maskCompareName(I.param);
                         });
INSTANTIATE_TEST_SUITE_P(PackedForms, MaskPackedForms,
                         testing::ValuesIn(maskPackedForms()),
                         [](const auto &I) {
                           return maskCompareName(I.param);
                         });
INSTANTIATE_TEST_SUITE_P(AlignedForms, MaskAlignedForms,
                         testing::ValuesIn(maskAlignedForms()),
                         [](const auto &I) {
                           return maskCompareName(I.param);
                         });

INSTANTIATE_TEST_SUITE_P(AllForms, MaskCompareForms,
                         testing::ValuesIn(maskCompareForms()),
                         [](const auto &Info) {
                           return maskCompareName(Info.param);
                         });

LowOp maskStateOp(unsigned Control, unsigned Bytes, unsigned Immediate,
                  uint32_t State, bool Memory = false) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(3, Bytes + 4);
  Op.addInput(
      NdVar::cst(unsigned(Memory ? Intrinsic::X86FPMaskCompareMemoryState
                                 : Intrinsic::X86FPMaskCompareState),
                 2));
  if (Memory)
    Op.addInput(NdVar::cst(0x2000, 8));
  Op.addInput(NdVar::cst(Control, 1));
  Op.addInput(NdVar::cst(Immediate, 1));
  Op.addInput(NdVar::tmp(1, Bytes));
  if (!Memory)
    Op.addInput(NdVar::tmp(2, Bytes));
  Op.addInput(NdVar::cst(State, 4));
  return Op;
}
unsigned maskControl(const MaskCompareForm &F) {
  return (F.Double ? 1 : 0) | (F.Scalar ? 2 : 0) | (F.Vex ? 4 : 0);
}
unsigned maskActiveBytes(const MaskCompareForm &F) {
  return F.Scalar ? (F.Double ? 8 : 4) : F.Width;
}
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
using MaskNativeFunction = std::vector<uint8_t> (*)(llvm::ArrayRef<uint8_t>,
                                                    llvm::ArrayRef<uint8_t>,
                                                    uint32_t);
template <unsigned Control, unsigned Width, unsigned Immediate>
__attribute__((target("avx"))) std::vector<uint8_t>
nativeMaskCompare(llvm::ArrayRef<uint8_t> Left, llvm::ArrayRef<uint8_t> Right,
                  uint32_t State) {
  using Element = std::conditional_t<bool(Control & 1), double, float>;
  using Vector = Element __attribute__((vector_size(Width)));
  using Value = std::conditional_t<bool(Control & 2), Element, Vector>;
  const uint32_t Saved = _mm_getcsr();
  Value A, B;
  std::memcpy(&A, Left.data(), sizeof(Value));
  std::memcpy(&B, Right.data(), sizeof(Value));
#define NATIVE_MASK(C, MN, EXTRA)                                              \
  if constexpr (Control == C)                                                  \
  __asm__ volatile("ldmxcsr %1\n\t" MN " $%c3,%2" EXTRA ",%0\n\tstmxcsr %1"    \
                   : "+x"(A), "+m"(State)                                      \
                   : "x"(B), "i"(Immediate)                                    \
                   : "memory")
  NATIVE_MASK(0, "cmpps", "");
  NATIVE_MASK(1, "cmppd", "");
  NATIVE_MASK(2, "cmpss", "");
  NATIVE_MASK(3, "cmpsd", "");
  NATIVE_MASK(4, "vcmpps", ",%0");
  NATIVE_MASK(5, "vcmppd", ",%0");
  NATIVE_MASK(6, "vcmpss", ",%0");
  NATIVE_MASK(7, "vcmpsd", ",%0");
#undef NATIVE_MASK
  _mm_setcsr(Saved);
  std::vector<uint8_t> Result(sizeof(Value) + 4);
  std::memcpy(Result.data(), &A, sizeof(Value));
  std::memcpy(Result.data() + sizeof(Value), &State, 4);
  return Result;
}
template <unsigned Control, unsigned Width, size_t... I>
constexpr auto nativeMaskTable(std::index_sequence<I...>) {
  return std::array<MaskNativeFunction, sizeof...(I)>{
      nativeMaskCompare<Control, Width, (I < 32 ? I : (I - 32) | 0xe0)>...};
}
const std::array<MaskNativeFunction, 64> &maskNativeTable(unsigned Control,
                                                          unsigned Width) {
#define TABLE(C, W)                                                            \
  if (Control == C && Width == W) {                                            \
    static constexpr auto T =                                                  \
        nativeMaskTable<C, W>(std::make_index_sequence<64>{});                 \
    return T;                                                                  \
  }
  TABLE(0, 16);
  TABLE(1, 16);
  TABLE(2, 16);
  TABLE(3, 16);
  TABLE(4, 16);
  TABLE(5, 16);
  TABLE(6, 16);
  TABLE(7, 16);
  TABLE(4, 32);
  TABLE(5, 32);
#undef TABLE
  llvm_unreachable("invalid native comparison form");
}
#endif
TEST(MaskCompareConcrete, EveryPredicateMaskAndInputControlMatchesNative) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  const uint64_t Singles[] = {0,          0x80000000, 1,          0x80000001,
                              0x007fffff, 0x00800000, 0x3f800000, 0xbf800000,
                              0x7f800000, 0xff800000, 0x7fc00031, 0x7f800071};
  const uint64_t Doubles[] = {0,
                              0x8000000000000000ULL,
                              1,
                              0x8000000000000001ULL,
                              0x000fffffffffffffULL,
                              0x0010000000000000ULL,
                              0x3ff0000000000000ULL,
                              0xbff0000000000000ULL,
                              0x7ff0000000000000ULL,
                              0xfff0000000000000ULL,
                              0x7ff8000000000031ULL,
                              0x7ff0000000000071ULL};
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (const auto &F : maskCompareForms()) {
    if (F.Memory)
      continue;
    const auto &Table = maskNativeTable(maskControl(F), F.Width);
    const unsigned Bytes = maskActiveBytes(F), Element = F.Double ? 8 : 4;
    for (unsigned P = 0; P < 64; ++P)
      for (unsigned Env = 0; Env < (P < 32 ? 128u : 4u); ++Env)
        for (unsigned A = 0; A < 12; ++A)
          for (unsigned B = 0; B < 12; ++B) {
            std::vector<uint8_t> Left(Bytes), Right(Bytes);
            for (unsigned Lane = 0; Lane < Bytes / Element; ++Lane) {
              const uint64_t L = F.Double ? Doubles[(A + Lane) % 12]
                                          : Singles[(A + Lane) % 12];
              const uint64_t R = F.Double ? Doubles[(B + Lane * 3) % 12]
                                          : Singles[(B + Lane * 3) % 12];
              std::memcpy(Left.data() + Lane * Element, &L, Element);
              std::memcpy(Right.data() + Lane * Element, &R, Element);
            }
            const uint32_t Base = 0x1f80 | ((Env & 3) << 13) |
                                  ((Env & 4) ? 0x40 : 0) |
                                  ((Env & 8) ? 0x8000 : 0);
            auto Expected = Table[P](Left, Right, Base);
            uint32_t Raised = 0;
            std::memcpy(&Raised, Expected.data() + Bytes, 4);
            Raised &= 0x3f;
            const uint32_t State =
                (Base & ~((Env & 32) ? 0x80 : 0) & ~((Env & 64) ? 0x100 : 0)) |
                ((Env & 16) ? 0x25 : 0);
            const bool Completes = !(Raised & ~(State >> 7) & 0x3f);
            if (Completes)
              Expected = Table[P](Left, Right, State);
            for (bool Strict : {false, true}) {
              SCOPED_TRACE(testing::Message()
                           << maskControl(F) << ':' << Bytes << ':' << P << ':'
                           << Env << ':' << A << ':' << B << ':' << Strict);
              NdOpEmulator E(Image);
              E.setStrictMode(Strict);
              E.setMXCSR(State);
              E.setRegisterBytes(1, Left);
              E.setRegisterBytes(2, Right);
              const std::vector<uint8_t> Old(Bytes + 4, 0x5a);
              E.setRegisterBytes(3, Old);
              const auto Op = maskStateOp(maskControl(F), Bytes,
                                          P < 32 ? P : (P - 32) | 0xe0, State);
              EXPECT_EQ(E.step(Op), Completes);
              EXPECT_EQ(E.getMXCSR(), State | Raised);
              ASSERT_TRUE(E.getRegisterBytes(3));
              EXPECT_EQ(*E.getRegisterBytes(3), Completes ? Expected : Old);
            }
          }
  }
#else
  GTEST_SKIP() << "native x64 GCC/Clang required";
#endif
}
TEST(MaskCompareConcrete,
     SourceExtentAlignmentPermissionsAndUnknownStateRefuseAtomically) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  for (const auto &F : maskCompareForms()) {
    if (!F.Memory)
      continue;
    const unsigned Control = maskControl(F), Bytes = maskActiveBytes(F);
    const std::vector<uint8_t> Left(Bytes, 0);
    std::vector<uint8_t> Right(Bytes, 0);
    Right[0] = 1;
    const auto Expected =
        maskNativeTable(Control, F.Width)[8](Left, Right, 0x1f80);
    for (auto Space :
         {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
          NdMemoryAddressSpace::X86GS})
      for (unsigned Mode = 0; Mode < 9; ++Mode)
        for (bool Strict : {false, true}) {
          BinaryImage Image;
          Image.Arch = Arch::X64;
          Image.Bits = Bitness::Bits64;
          Segment S;
          S.VA = 0x2000;
          S.Size = S.FileSz = Bytes - (Mode == 1 ? 1 : 0) + (Mode == 8 ? 1 : 0);
          S.Flags = Mode == 2 ? SegmentFlags::Writable : SegmentFlags::Readable;
          S.Data.assign(S.Size, 0);
          S.Data[Mode == 8 ? 1 : 0] = 1;
          Image.Segments.push_back(S);
          NdOpEmulator E(Image);
          E.setStrictMode(Strict);
          if (Mode != 4)
            E.setX86LinearAddressBits(48);
          if (Space != NdMemoryAddressSpace::Default && Mode != 3)
            E.setMemoryAddressSpaceBase(Space, Mode == 6 ? 0x8000 : 0x1000);
          E.setRegisterBytes(1, Left);
          const std::vector<uint8_t> Old(Bytes + 4, 0x5a);
          E.setRegisterBytes(3, Old);
          auto Op = maskStateOp(Control, Bytes, 8, 0x1f80, true);
          Op.MemoryAddressSpace = Space;
          uint64_t Address = 0x2000 + (Mode == 8 ? 1 : 0);
          if (Mode == 5)
            Address = UINT64_C(0x0000800000000000);
          else if (Space != NdMemoryAddressSpace::Default)
            Address -= Mode == 6 ? 0x8000 : 0x1000;
          Op.Inputs[1].Offset = Address;
          if (Mode == 7)
            Op.Inputs[5].Offset = 0x1f81;
          const bool Completes =
              Mode == 0 || Mode == 6 ||
              (Mode == 3 && Space == NdMemoryAddressSpace::Default) ||
              (Mode == 8 && x86FPMaskCompareMemoryAlignment(Control) == 1);
          EXPECT_EQ(E.step(Op), Completes);
          EXPECT_EQ(E.getMXCSR(), Completes ? 0x1f82u : 0x1f80u);
          ASSERT_TRUE(E.getRegisterBytes(3));
          EXPECT_EQ(*E.getRegisterBytes(3), Completes ? Expected : Old);
        }
  }
  BinaryImage Image;
  Image.Arch = Arch::X64;
  NdOpEmulator E(Image);
  E.setCallPreservedRegisters({1, 2});
  E.setRegister(1, 0);
  E.setRegister(2, 0);
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(Entry, 8));
  ASSERT_TRUE(E.step(Call));
  EXPECT_FALSE(E.step(maskStateOp(2, 4, 0, 0x1f80)));
  EXPECT_FALSE(E.getRegisterBytes(3));
#else
  GTEST_SKIP() << "native x64 GCC/Clang oracle required";
#endif
}
TEST(MaskCompareContract, RawMasksAndMalformedLowMedHighShapesRemainExact) {
  for (const auto &F : maskCompareForms()) {
    const unsigned Bytes = maskActiveBytes(F), Control = maskControl(F);
    auto Low = maskStateOp(Control, Bytes, 255, 0x1f80, F.Memory);
    const auto Id = static_cast<Intrinsic>(Low.Inputs[0].Offset);
    ASSERT_TRUE(x86FPStateShapeIsValid(Id, x86FPStateLowShape(Low, Arch::X64)));
    EXPECT_EQ(x86FPStateMemoryBytes(Id, Bytes + 4, Control),
              F.Memory ? Bytes : 0u);
    EXPECT_EQ(x86FPStateNumericalSliceSize(
                  Id, x86FPStateLowShape(Low, Arch::X64), 0, Bytes),
              0u);
    EXPECT_EQ(x86FPMaskCompareStateImmediate(x86FPStateHelperLayout(
                  Id, x86FPStateLowShape(Low, Arch::X64))),
              255u);
    for (unsigned Mode = 0; Mode < 11; ++Mode) {
      auto Bad = Low;
      switch (Mode) {
      case 0:
        Bad.Output.Size += 4;
        break;
      case 1:
        Bad.Inputs[F.Memory ? 2 : 1].Offset = 8;
        break;
      case 2:
        Bad.Inputs[F.Memory ? 2 : 1].Size = 2;
        break;
      case 3:
        Bad.Inputs[F.Memory ? 3 : 2].Size = 2;
        break;
      case 4:
        Bad.Inputs[F.Memory ? 3 : 2].Offset = 256;
        break;
      case 5:
        Bad.Inputs[5].Size = 8;
        break;
      case 6:
        Bad.Inputs[F.Memory ? 4 : 3].Size = Bytes + 1;
        break;
      case 7:
        Bad.Inputs[0].Size = 1;
        break;
      case 8:
        Bad.NumInputs = 5;
        break;
      case 9:
        Bad.Output.Space = VnodeSpace::CONST;
        break;
      case 10:
        Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      }
      EXPECT_FALSE(
          x86FPStateShapeIsValid(Id, x86FPStateLowShape(Bad, Arch::X64)));
      BinaryImage Image;
      Image.Arch = Arch::X64;
      NdOpEmulator E(Image);
      const std::vector<uint8_t> Old(Bytes + 4, 0x5a);
      E.setRegisterBytes(3, Old);
      EXPECT_FALSE(E.step(Bad));
      ASSERT_TRUE(E.getRegisterBytes(3));
      EXPECT_EQ(*E.getRegisterBytes(3), Old);
      EXPECT_EQ(E.getMXCSR(), 0x1f80u);
    }
    MedOp M;
    M.Opcode = NdOp::INTRINSIC;
    M.Output = {.Kind = MedVar::Temp,
                .TheArch = Arch::X64,
                .Id = 3,
                .Size = static_cast<uint16_t>(Bytes + 4)};
    std::vector<ExprPtr> Operands;
    for (unsigned I = 0; I < Low.NumInputs; ++I) {
      M.addInput(
          MedVar::makeConst(Low.Inputs[I].isConst() ? Low.Inputs[I].Offset : 0,
                            Low.Inputs[I].Size));
      if (I)
        Operands.push_back(HighExpr::makeConst(
            Low.Inputs[I].isConst() ? Low.Inputs[I].Offset : 0,
            Low.Inputs[I].Size));
    }
    EXPECT_TRUE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(M, Arch::X64)));
    M.IntrinsicOutputs.push_back(M.Output);
    EXPECT_FALSE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(M, Arch::X64)));
    auto H = HighExpr::makeCall("", 0, std::move(Operands));
    H->IntrinsicId = Id;
    H->Type = NdType::makeInt(Bytes + 4, false);
    ASSERT_TRUE(x86FPStateShapeIsValid(Id, x86FPStateHighShape(*H, Arch::X64)));
    H->IsIndirectCall = true;
    EXPECT_FALSE(
        x86FPStateShapeIsValid(Id, x86FPStateHighShape(*H, Arch::X64)));
    H->IsIndirectCall = false;
    const unsigned LeftIndex = F.Memory ? 3 : 2;
    H->Operands[LeftIndex]->Type = NdType::makeFloat(Bytes);
    EXPECT_EQ(x86FPStateShapeIsValid(Id, x86FPStateHighShape(*H, Arch::X64)),
              F.Scalar);
    H->Operands[LeftIndex]->Type = NdType::makeInt(Bytes, false);
    H->Operands[4]->Type = NdType::makeFloat(4);
    EXPECT_FALSE(
        x86FPStateShapeIsValid(Id, x86FPStateHighShape(*H, Arch::X64)));
  }
}
TEST(MaskCompareContract,
     OwnedAssemblyAuthenticatesFullImmediateTypesAndEffects) {
  for (const auto &F : maskCompareForms())
    for (unsigned Immediate : {0u, 8u, 16u, 31u, 128u, 255u}) {
      llvm::LLVMContext C;
      llvm::Module Module("maskcmp", C);
      llvm::IRBuilder<> B(C);
      const unsigned Layout = x86FPMaskCompareStateLayout(
          maskActiveBytes(F), maskControl(F), Immediate);
      auto *T = x86FPMaskCompareLLVMType(C, Layout);
      auto *Ty = llvm::FunctionType::get(
          T, {T, F.Memory ? B.getPtrTy() : T, B.getPtrTy()}, false);
      auto *Fn = llvm::Function::Create(Ty, llvm::Function::ExternalLinkage,
                                        "probe", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
      std::vector<llvm::Value *> Args;
      for (auto &Arg : Fn->args())
        Args.push_back(&Arg);
      auto *Asm =
          llvm::InlineAsm::get(Ty, x86FPMaskCompareAsm(Layout, F.Memory),
                               x86FPMaskCompareConstraints(F.Memory), true);
      auto *Call = B.CreateCall(Asm, Args);
      Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(C, {}));
      Call->setMetadata(X86FPMaskCompareAsmMetadata,
                        llvm::MDNode::get(C, llvm::ConstantAsMetadata::get(
                                                 B.getInt32(Layout))));
      EXPECT_EQ(classifyX86FPStateAsm(*Call),
                std::make_pair(F.Memory ? Intrinsic::X86FPMaskCompareMemoryState
                                        : Intrinsic::X86FPMaskCompareState,
                               Layout));
      // A forged predicate layout cannot authorize the original asm.
      Call->setMetadata(
          X86FPMaskCompareAsmMetadata,
          llvm::MDNode::get(C, llvm::ConstantAsMetadata::get(
                                   B.getInt32(Layout ^ (1u << 16)))));
      EXPECT_DEATH_IF_SUPPORTED(classifyX86FPStateAsm(*Call),
                                "invalid x86 FP state");
      Call->setMetadata(X86FPMaskCompareAsmMetadata,
                        llvm::MDNode::get(C, llvm::ConstantAsMetadata::get(
                                                 B.getInt32(Layout))));
      Call->setCalledOperand(
          llvm::InlineAsm::get(Ty, x86FPMaskCompareAsm(Layout, F.Memory),
                               x86FPMaskCompareConstraints(F.Memory), false));
      EXPECT_DEATH_IF_SUPPORTED(classifyX86FPStateAsm(*Call),
                                "invalid x86 FP state");
    }
}

std::vector<uint8_t> singleMaskBytes(const MaskCompareForm &F,
                                     unsigned Immediate, unsigned Dst = 0,
                                     unsigned Left = 0, unsigned Right = 1) {
  const unsigned PP = F.Scalar ? (F.Double ? 3 : 2) : (F.Double ? 1 : 0);
  std::vector<uint8_t> Bytes;
  if (F.Vex) {
    const unsigned P0 =
        (Dst & 8 ? 0 : 0x80) | 0x40 | (F.Memory || !(Right & 8) ? 0x20 : 0) | 1;
    Bytes = {0xc4, uint8_t(P0),
             uint8_t(((~Left & 15) << 3) | PP | (F.Width == 32 ? 4 : 0))};
  } else {
    if (PP)
      Bytes.push_back(PP == 1 ? 0x66 : PP == 2 ? 0xf3 : 0xf2);
    const unsigned Rex =
        0x40 | (Dst & 8 ? 4 : 0) | (!F.Memory && (Right & 8) ? 1 : 0);
    if (Rex != 0x40)
      Bytes.push_back(Rex);
    Bytes.push_back(0x0f);
  }
  Bytes.push_back(0xc2);
  Bytes.push_back(uint8_t((F.Memory ? 0x40 : 0xc0) | ((Dst & 7) << 3) |
                          (F.Memory ? 0 : Right & 7)));
  if (F.Memory)
    Bytes.push_back(32);
  Bytes.push_back(uint8_t(Immediate));
  return Bytes;
}
TEST(MaskCompareContract, AllImmediateBytesAndMetadataAreAuthenticated) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (const auto &F : maskCompareForms())
      for (unsigned Immediate = 0; Immediate < 256; ++Immediate) {
        const auto Bytes = singleMaskBytes(F, Immediate);
        Decoder D;
        ASSERT_TRUE(D.init(A));
        DecodedInsn I;
        ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I),
                  int(Bytes.size()));
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        unsigned Found = 0;
        for (const auto &Op : Ops)
          if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
              isX86FPMaskCompareStateIntrinsic(
                  static_cast<Intrinsic>(Op.Inputs[0].Offset))) {
            ++Found;
            const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
            const auto Shape = x86FPStateLowShape(Op, A);
            EXPECT_TRUE(x86FPStateShapeIsValid(Id, Shape));
            EXPECT_EQ(Shape.Immediate, Immediate);
            EXPECT_EQ(Shape.Control, maskControl(F));
            EXPECT_EQ(Op.Output.Size, maskActiveBytes(F) + 4);
          }
        EXPECT_EQ(Found, 1u);
        if (Immediate != 8 && Immediate != 255)
          continue;
        for (unsigned Mode = 0; Mode < 9; ++Mode) {
          DecodedInsn Bad;
          ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Bad),
                    int(Bytes.size()));
          auto &X = Bad.Raw->detail->x86;
          switch (Mode) {
          case 0:
            X.operands[0].size = 4;
            break;
          case 1:
            X.encoding.imm_size = 0;
            break;
          case 2:
            X.encoding.imm_offset += 1;
            break;
          case 3:
            X.modrm ^= 1;
            break;
          case 4:
            X.avx_sae = true;
            break;
          case 5:
            X.op_count = 0;
            break;
          case 6:
            if (F.Vex)
              X.avx_cc = static_cast<x86_avx_cc>((Immediate & 31) + 2);
            else
              X.sse_cc = static_cast<x86_sse_cc>((Immediate & 7) + 2);
            break;
          case 7:
            X.operands[F.Vex ? 2 : 1].size += 1;
            break;
          case 8:
            Bad.Raw->bytes[F.Vex ? 3 : (F.Double || F.Scalar) ? 2 : 1] = 0xc3;
            break;
          }
          std::vector<LowOp> Rejected;
          EXPECT_THROW(D.liftToLow(Bad, Rejected), UnliftedInstruction);
        }
      }
}
TEST(MaskCompareContract, PhysicalRegistersAndAliasesRetainAuthenticatedRoles) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  for (const auto &F : maskCompareForms()) {
    if (F.Memory)
      continue;
    for (unsigned Dst = 0; Dst < 16; ++Dst)
      for (unsigned Left = 0; Left < 16; ++Left) {
        if (!F.Vex && Left != Dst)
          continue;
        for (unsigned Right = 0; Right < 16; ++Right) {
          const auto Bytes = singleMaskBytes(F, 8, Dst, Left, Right);
          DecodedInsn I;
          ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I),
                    int(Bytes.size()));
          std::vector<LowOp> Ops;
          ASSERT_NO_THROW(D.liftToLow(I, Ops));
          unsigned Found = 0;
          for (const auto &Op : Ops)
            if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                Op.Inputs[0].Offset ==
                    unsigned(Intrinsic::X86FPMaskCompareState)) {
              ++Found;
              EXPECT_EQ(Op.Output.Size, maskActiveBytes(F) + 4);
              EXPECT_TRUE(
                  x86FPStateShapeIsValid(Intrinsic::X86FPMaskCompareState,
                                         x86FPStateLowShape(Op, Arch::X64)));
            }
          EXPECT_EQ(Found, 1u);
        }
      }
  }
}

} // namespace
