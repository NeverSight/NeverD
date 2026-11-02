//===- X86_64_ComiStateTests.cpp - COMI numerical/state checks ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original scalar flag comparisons versus complete flag/MXCSR observations.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/backend/llvm/X86/LLVMX86FPStateAsm.h"
#include "neverd/ir/high/X86/X86FPStateShape.h"

#include <array>
#include <type_traits>

namespace {
struct ComiForm {
  bool Double, Quiet, Vex, Memory;
};
std::vector<ComiForm> comiForms() {
  std::vector<ComiForm> Forms;
  for (bool Double : {false, true})
    for (bool Quiet : {false, true})
      for (bool Vex : {false, true})
        for (bool Memory : {false, true})
          Forms.push_back({Double, Quiet, Vex, Memory});
  return Forms;
}
std::string comiName(const ComiForm &F) {
  return std::string(F.Vex ? "Vex" : "Legacy") + (F.Quiet ? "Ucomi" : "Comi") +
         (F.Double ? "SD" : "SS") + (F.Memory ? "Memory" : "Register");
}
std::vector<uint8_t> comiBytes(const ComiForm &F, bool Pointer = false,
                               bool GS = false) {
#if defined(_WIN32)
  constexpr uint8_t Base = 1;
#else
  constexpr uint8_t Base = 7;
#endif
  // LAHF writes only AH. Define the rest of the fixture's return register so
  // source publication never needs to invent an untouched entry RAX value.
  std::vector<uint8_t> B{0x31, 0xc0, 0xc5,
                         0xfc, 0x10, uint8_t(Base), // vmovups ymm0,[arg]
                         0x0f, 0xae, uint8_t(0x50 | Base),
                         32}; // ldmxcsr
  B.insert(B.end(), {0xc5, 0xfc, 0x10, uint8_t(0x48 | Base), 16});
  if (Pointer)
    B.insert(B.end(),
             {0x4c, 0x8b, uint8_t(0x58 | Base), 64}); // mov r11,[arg+64]
  if (GS)
    B.push_back(0x65);
  if (F.Vex) {
    if (Pointer)
      B.insert(B.end(), {0xc4, 0xc1, uint8_t(F.Double ? 0xf9 : 0xf8)});
    else
      B.insert(B.end(), {0xc5, uint8_t(F.Double ? 0xf9 : 0xf8)});
  } else {
    if (F.Double)
      B.push_back(0x66);
    if (Pointer)
      B.push_back(0x41);
    B.push_back(0x0f);
  }
  B.push_back(F.Quiet ? 0x2e : 0x2f);
  if (Pointer)
    B.push_back(0x03);
  else if (F.Memory)
    B.insert(B.end(), {uint8_t(0x40 | Base), 16});
  else
    B.push_back(0xc1);
  for (const auto [Opcode, Offset] :
       {std::pair{0x92, 40}, std::pair{0x94, 41}, std::pair{0x9a, 42},
        std::pair{0x90, 43}, std::pair{0x98, 44}})
    B.insert(B.end(),
             {0x0f, uint8_t(Opcode), uint8_t(0x40 | Base), uint8_t(Offset)});
  B.insert(B.end(), {0x9f,
                     0x88,
                     uint8_t(0x60 | Base),
                     45, // lahf; mov [arg+45],ah
                     0x0f,
                     0xae,
                     uint8_t(0x58 | Base),
                     36,
                     0xc5,
                     0xfc,
                     0x11,
                     uint8_t(0x40 | Base),
                     80,
                     0xc5,
                     0xfc,
                     0x11,
                     uint8_t(0x88 | Base),
                     112,
                     0,
                     0,
                     0,
                     0xc3});
  return B;
}

LowOp comiOp(unsigned Control, uint32_t State, bool Memory = false) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(3, 8);
  Op.addInput(NdVar::cst(unsigned(Memory ? Intrinsic::X86FPComiMemoryState
                                         : Intrinsic::X86FPComiState),
                         2));
  if (Memory)
    Op.addInput(NdVar::cst(0x2000, 8));
  Op.addInput(NdVar::cst(Control, 1));
  Op.addInput(NdVar::tmp(1, x86FPComiElementBytes(Control)));
  if (!Memory)
    Op.addInput(NdVar::tmp(2, x86FPComiElementBytes(Control)));
  Op.addInput(NdVar::cst(State, 4));
  return Op;
}
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
template <unsigned Control>
__attribute__((target("avx"))) std::pair<uint32_t, uint32_t>
nativeComi(uint64_t A, uint64_t B, uint32_t State) {
  const auto Saved = _mm_getcsr();
  using T = std::conditional_t<bool(Control & 2), double, float>;
  T Left, Right;
  std::memcpy(&Left, &A, sizeof(T));
  std::memcpy(&Right, &B, sizeof(T));
  uint32_t Flags;
#define ORACLE(N, MN)                                                          \
  if constexpr (Control == N)                                                  \
  __asm__ volatile("ldmxcsr %1\n\t" MN                                         \
                   " %3,%2\n\tlahf\n\tmovzbl %%ah,%%eax\n\tstmxcsr %1"         \
                   : "=&a"(Flags), "+m"(State)                                 \
                   : "x"(Left), "x"(Right)                                     \
                   : "cc", "memory")
  ORACLE(0, "comiss");
  ORACLE(1, "ucomiss");
  ORACLE(2, "comisd");
  ORACLE(3, "ucomisd");
  ORACLE(4, "vcomiss");
  ORACLE(5, "vucomiss");
  ORACLE(6, "vcomisd");
  ORACLE(7, "vucomisd");
#undef ORACLE
  _mm_setcsr(Saved);
  return {Flags, State};
}
constexpr auto NativeComparisons =
    std::array{nativeComi<0>, nativeComi<1>, nativeComi<2>, nativeComi<3>,
               nativeComi<4>, nativeComi<5>, nativeComi<6>, nativeComi<7>};
#endif
TEST(ComiConcrete, AllRelationsExceptionsAndControlModesMatchNative) {
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
  for (unsigned Control = 0; Control < 8; ++Control) {
    const llvm::ArrayRef<uint64_t> Inputs =
        Control & 2 ? llvm::ArrayRef<uint64_t>(Doubles)
                    : llvm::ArrayRef<uint64_t>(Singles);
    for (unsigned Env = 0; Env < 128; ++Env)
      for (auto A : Inputs)
        for (auto B : Inputs) {
          const uint32_t Base = 0x1f80 | ((Env & 3) << 13) |
                                ((Env & 4) ? 0x40 : 0) |
                                ((Env & 8) ? 0x8000 : 0);
          const auto Expected = NativeComparisons[Control](A, B, Base);
          const uint32_t State =
              (Base & ~((Env & 32) ? 0x80 : 0) & ~((Env & 64) ? 0x100 : 0)) |
              ((Env & 16) ? 0x25 : 0);
          const uint32_t Raised = Expected.second & 0x3f;
          const bool Completes = !(Raised & ~(State >> 7) & 0x3f);
          // Execute every normally completing incoming mask/sticky state on
          // hardware. Faulting cases use the masked exception classification
          // here and the independent native fault children below.
          const auto CompletedNative =
              Completes ? NativeComparisons[Control](A, B, State) : Expected;
          for (bool Strict : {false, true}) {
            SCOPED_TRACE(testing::Message() << Control << ':' << Env << ':' << A
                                            << ':' << B << ':' << Strict);
            NdOpEmulator E(Image);
            E.setStrictMode(Strict);
            E.setMXCSR(State);
            E.setRegister(1, A);
            E.setRegister(2, B);
            E.setRegister(3, UINT64_C(0x123456789abcdef0));
            EXPECT_EQ(E.step(comiOp(Control, State)), Completes);
            EXPECT_EQ(E.getMXCSR(),
                      Completes ? CompletedNative.second : State | Raised);
            EXPECT_EQ(E.getRegister(3),
                      Completes ? uint64_t(CompletedNative.first) |
                                      (uint64_t(CompletedNative.second) << 32)
                                : UINT64_C(0x123456789abcdef0));
          }
        }
  }
#else
  GTEST_SKIP() << "native x64 GCC/Clang oracle required";
#endif
}
TEST(ComiContract, FlagAggregatesRemainRawAndMalformedShapesRefuse) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned Control = 0; Control < 8; ++Control)
      for (bool Memory : {false, true}) {
        auto Op = comiOp(Control, 0x1f80, Memory);
        const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
        const auto Shape = x86FPStateLowShape(Op, A);
        ASSERT_TRUE(x86FPStateShapeIsValid(Id, Shape));
        EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 0, 4), 0u);
        EXPECT_EQ(x86FPStateMemoryBytes(Id, 8, Control),
                  Memory ? x86FPComiElementBytes(Control) : 0u);
        for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
          auto Bad = Op;
          switch (Mutation) {
          case 0:
            Bad.Output.Size = 12;
            break;
          case 1:
            Bad.NumInputs = 4;
            break;
          case 2:
            Bad.Inputs[Memory ? 2 : 1].Offset = 8;
            break;
          case 3:
            Bad.Inputs[Memory ? 2 : 1].Size = 2;
            break;
          case 4:
            Bad.Inputs[4].Size = 8;
            break;
          case 5:
            Bad.Inputs[Memory ? 3 : 2].Size = 16;
            break;
          case 6:
            Bad.Inputs[0].Size = 1;
            break;
          case 7:
            Bad.Output.Space = VnodeSpace::CONST;
            break;
          }
          EXPECT_FALSE(x86FPStateShapeIsValid(Id, x86FPStateLowShape(Bad, A)));
          BinaryImage Image;
          Image.Arch = A;
          NdOpEmulator E(Image);
          E.setRegister(3, UINT64_C(0x123456789abcdef0));
          EXPECT_FALSE(E.step(Bad));
          EXPECT_EQ(E.getRegister(3), UINT64_C(0x123456789abcdef0));
          EXPECT_EQ(E.getMXCSR(), 0x1f80u);
        }
      }
}
TEST(ComiConcrete, MemoryExtentsPermissionsSegmentsAndUnknownStateAreExact) {
  for (unsigned Control : {0U, 2U})
    for (auto Space :
         {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
          NdMemoryAddressSpace::X86GS})
      for (unsigned Mode = 0; Mode < 7; ++Mode)
        for (bool Strict : {false, true}) {
          const unsigned Bytes = x86FPComiElementBytes(Control);
          BinaryImage Image;
          Image.Arch = Arch::X64;
          Image.Bits = Bitness::Bits64;
          Segment S;
          S.VA = 0x2001;
          S.Size = S.FileSz = Bytes - (Mode == 1 ? 1 : 0);
          S.Flags = Mode == 2 ? SegmentFlags::Writable : SegmentFlags::Readable;
          S.Data.assign(S.Size, 0);
          S.Data[0] = 1;
          Image.Segments.push_back(S);
          NdOpEmulator E(Image);
          E.setStrictMode(Strict);
          E.setRegister(1, 0);
          E.setRegister(3, UINT64_C(0x123456789abcdef0));
          if (Mode != 4)
            E.setX86LinearAddressBits(48);
          if (Space != NdMemoryAddressSpace::Default && Mode != 3)
            E.setMemoryAddressSpaceBase(Space, Mode == 6 ? 0x8000 : 0x1000);
          auto Op = comiOp(Control, 0x1f80, true);
          Op.MemoryAddressSpace = Space;
          Op.Inputs[1].Offset = Mode == 5 ? UINT64_C(0x0000800000000000)
                                : Space == NdMemoryAddressSpace::Default
                                    ? 0x2001
                                : Mode == 6 ? UINT64_C(0x2001) - 0x8000
                                            : 0x1001;
          const bool Completes =
              Mode == 0 || Mode == 6 ||
              (Mode == 3 && Space == NdMemoryAddressSpace::Default);
          EXPECT_EQ(E.step(Op), Completes);
          EXPECT_EQ(E.getMXCSR(), Completes ? 0x1f82u : 0x1f80u);
          EXPECT_EQ(E.getRegister(3), Completes ? UINT64_C(0x1f8200000003)
                                                : UINT64_C(0x123456789abcdef0));
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
  EXPECT_FALSE(E.step(comiOp(0, 0x1f80)));
  EXPECT_FALSE(E.getRegister(3));
}
TEST(ComiContract,
     RawBytesAndDecodedMetadataAgreeAcrossRegisterAndAddressForms) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned Control = 0; Control < 8; ++Control)
      for (bool Memory : {false, true}) {
        const bool Vex = Control & 4, Double = Control & 2, Quiet = Control & 1;
        std::vector<uint8_t> Bytes;
        if (Vex)
          Bytes = {0xc5, uint8_t(Double ? 0xf9 : 0xf8)};
        else {
          if (Double)
            Bytes.push_back(0x66);
          Bytes.push_back(0x0f);
        }
        Bytes.push_back(Quiet ? 0x2e : 0x2f);
        Bytes.push_back(Memory ? 0x40 : 0xc1);
        if (Memory)
          Bytes.push_back(16);
        Decoder D;
        ASSERT_TRUE(D.init(A));
        DecodedInsn Insn;
        ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Insn),
                  int(Bytes.size()));
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
        unsigned Found = 0;
        for (const auto &Op : Ops)
          if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
              isX86FPComiStateIntrinsic(
                  static_cast<Intrinsic>(Op.Inputs[0].Offset))) {
            ++Found;
            EXPECT_TRUE(x86FPStateShapeIsValid(
                static_cast<Intrinsic>(Op.Inputs[0].Offset),
                x86FPStateLowShape(Op, A)));
          }
        EXPECT_EQ(Found, 1u);
        for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
          DecodedInsn Bad;
          ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Bad),
                    int(Bytes.size()));
          auto &X = Bad.Raw->detail->x86;
          if (Mutation == 0)
            X.avx_sae = true;
          if (Mutation == 1)
            X.op_count = 3;
          if (Mutation == 2)
            X.operands[0].size = 8;
          if (Mutation == 3)
            X.operands[1].size = 32;
          if (Mutation == 4)
            X.modrm ^= 1;
          if (Mutation == 5)
            X.encoding.imm_size = 1;
          std::vector<LowOp> Rejected;
          EXPECT_THROW(D.liftToLow(Bad, Rejected), UnliftedInstruction);
        }
      }
}
TEST(ComiContract, MedHighAndOwnedAssemblyKeepOperandRolesAndRawFlags) {
  for (unsigned Control = 0; Control < 8; ++Control)
    for (bool Memory : {false, true}) {
      auto Low = comiOp(Control, 0x1f80, Memory);
      const auto Id = static_cast<Intrinsic>(Low.Inputs[0].Offset);
      MedOp M;
      M.Opcode = NdOp::INTRINSIC;
      M.Output = {
          .Kind = MedVar::Temp, .TheArch = Arch::X64, .Id = 3, .Size = 8};
      std::vector<ExprPtr> Inputs;
      for (unsigned I = 0; I < Low.NumInputs; ++I) {
        M.addInput(MedVar::makeConst(
            Low.Inputs[I].isConst() ? Low.Inputs[I].Offset : 0,
            Low.Inputs[I].Size));
        if (I)
          Inputs.push_back(HighExpr::makeConst(
              Low.Inputs[I].isConst() ? Low.Inputs[I].Offset : 0,
              Low.Inputs[I].Size));
      }
      ASSERT_TRUE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(M, Arch::X64)));
      M.IntrinsicOutputs.push_back(M.Output);
      EXPECT_FALSE(
          x86FPStateShapeIsValid(Id, x86FPStateMedShape(M, Arch::X64)));
      auto H = HighExpr::makeCall("", 0, std::move(Inputs));
      H->IntrinsicId = Id;
      H->Type = NdType::makeInt(8, false);
      ASSERT_TRUE(
          x86FPStateShapeIsValid(Id, x86FPStateHighShape(*H, Arch::X64)));
      H->IsIndirectCall = true;
      EXPECT_FALSE(
          x86FPStateShapeIsValid(Id, x86FPStateHighShape(*H, Arch::X64)));
      llvm::LLVMContext C;
      llvm::Module Module("comi", C);
      llvm::IRBuilder<> B(C);
      auto *T = Control & 2 ? B.getDoubleTy() : B.getFloatTy();
      std::vector<llvm::Type *> Types{T, Memory ? B.getPtrTy() : T,
                                      B.getPtrTy()};
      auto *Ty = llvm::FunctionType::get(B.getInt32Ty(), Types, false);
      auto *F = llvm::Function::Create(Ty, llvm::Function::ExternalLinkage,
                                       "probe", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
      std::vector<llvm::Value *> Args;
      for (auto &Arg : F->args())
        Args.push_back(&Arg);
      const auto Layout =
          x86FPRoundStateLayout(x86FPComiElementBytes(Control), Control, 0);
      auto *Asm = llvm::InlineAsm::get(Ty, x86FPComiAsm(Layout, Memory),
                                       x86FPComiConstraints(Memory), true);
      auto *Call = B.CreateCall(Asm, Args);
      Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(C, {}));
      EXPECT_EQ(classifyX86FPStateAsm(*Call), std::make_pair(Id, Layout));
      Call->setCalledOperand(llvm::InlineAsm::get(
          Ty, x86FPComiAsm(Layout, Memory), "=&{ax},x,x,r", true));
      EXPECT_DEATH_IF_SUPPORTED(classifyX86FPStateAsm(*Call),
                                "invalid x86 FP state");
    }
}
class ComiFixture : public X86FPStateFixture {
protected:
  unsigned FaultMode = 0;
  bool PageProbe = false;
  bool GSProbe = false;
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
  return pages && VirtualProtect(pages+page_size,page_size,PAGE_NOACCESS,&old);
}
#else
#include <sys/mman.h>
#include <unistd.h>
static unsigned char *pages;
static size_t page_size;
static int allocate_pages(void) {
  page_size=sysconf(_SC_PAGESIZE);
  pages=mmap(0,page_size*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  return pages!=MAP_FAILED && !mprotect(pages+page_size,page_size,PROT_NONE);
}
#endif
)";
  }

  std::string comiFaultDriver(bool Double, bool Declare, bool Native) {
    const bool Denormal = FaultMode == 1;
    const bool Page = FaultMode == 2;
    const unsigned Incoming = Page ? 0x1f80 : Denormal ? 0x1e80 : 0x1f00;
    const unsigned Expected = Page ? Incoming : Incoming | (Denormal ? 2 : 1);
    const uint64_t Left = Denormal ? 1 : 0;
    const uint64_t Right = Denormal ? 0
                           : Double ? UINT64_C(0x7ff0000000000071)
                                    : 0x7f800071;
    std::string Text =
        "#define _GNU_SOURCE\n#include <stdint.h>\n#include "
        "<stdio.h>\n#include <string.h>\n#include <stdlib.h>\n#include "
        "<immintrin.h>\nextern void native_probe(uintptr_t);\n";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += "#define EXPECTED_CSR " + std::to_string(Expected) + "u\n";
    Text += "#define PAGE_FAULT " + std::to_string(Page) + "\n";
    if (Page)
      Text += pageAllocation();
    Text += R"(
static unsigned char actual[160],before[160];
static int valid(unsigned csr) {
  if (csr != EXPECTED_CSR) {printf("CSR expected=%08x actual=%08x\n",EXPECTED_CSR,csr);return 0;}
  return !memcmp(actual,before,sizeof(actual));
}
#if defined(_WIN32)
#include <windows.h>
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *info) {
  if (info->ExceptionRecord->ExceptionCode !=
      (PAGE_FAULT ? EXCEPTION_ACCESS_VIOLATION : EXCEPTION_FLT_INVALID_OPERATION))
    return EXCEPTION_CONTINUE_SEARCH;
#if PAGE_FAULT
  if(info->ExceptionRecord->NumberParameters != 2 ||
     info->ExceptionRecord->ExceptionInformation[0] != 0 ||
     info->ExceptionRecord->ExceptionInformation[1] < (uintptr_t)(pages+page_size) ||
     info->ExceptionRecord->ExceptionInformation[1] >= (uintptr_t)(pages+page_size+8))
    ExitProcess(2);
#endif
  ExitProcess(valid(info->ContextRecord->MxCsr) ? 0 : 1);
}
#elif defined(__linux__)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
static void on_fault(int sig,siginfo_t *info,void *context) {
  ucontext_t *u=(ucontext_t*)context;
  _exit(sig == (PAGE_FAULT ? SIGSEGV : SIGFPE) && valid(u->uc_mcontext.fpregs->mxcsr) ? 0 : 1);
}
#endif
int main(void) {
#if defined(_WIN32)
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (!AddVectoredExceptionHandler(1,on_fault)) return 77;
#else
  struct sigaction action={0}; action.sa_sigaction=on_fault;
  action.sa_flags=SA_SIGINFO; sigemptyset(&action.sa_mask);
  if(sigaction(PAGE_FAULT ? SIGSEGV : SIGFPE,&action,0)) return 77;
#endif
  memset(actual,0xa5,sizeof(actual));
)";
    Text += "  const uint64_t left=" + std::to_string(Left) +
            "ULL,right=" + std::to_string(Right) + "ULL;\n";
    Text += "  const uint32_t state=" + std::to_string(Incoming) + "u;\n";
    Text += "  memcpy(actual,&left," + std::to_string(Double ? 8 : 4) +
            "); memcpy(actual+16,&right," + std::to_string(Double ? 8 : 4) +
            ");\n";
    if (Page)
      Text += "  if(!allocate_pages()) return 77; uintptr_t "
              "address=(uintptr_t)(pages+page_size-1); "
              "memcpy(actual+64,&address,sizeof(address));\n";
    Text +=
        "  memcpy(actual+32,&state,4); memcpy(before,actual,sizeof(actual));\n";
    Text += Native ? "  native_probe((uintptr_t)actual);\n"
                   : "  isa_probe((uintptr_t)actual);\n";
    Text += "  _mm_setcsr(0x1f80); return 99; /* missing exception */\n}\n";
    return Text;
  }
  std::string comparisonDriver(bool Double, bool DeclareProbe, bool Fault,
                               bool Native = false) override {
    if (Fault)
      return comiFaultDriver(Double, DeclareProbe, Native);
    std::string Text = R"(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <immintrin.h>
extern void native_probe(uintptr_t);
)";
    if (DeclareProbe)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += Double ? R"(
static const uint64_t inputs[] = {0,0x8000000000000000ULL,1,
  0x8000000000000001ULL,0x000fffffffffffffULL,0x0010000000000000ULL,
  0x3ff0000000000000ULL,0xbff0000000000000ULL,0x7ff0000000000000ULL,
  0xfff0000000000000ULL,0x7ff8000000000031ULL,0x7ff0000000000071ULL};
)"
                   : R"(
static const uint32_t inputs[] = {0,0x80000000,1,0x80000001,0x007fffff,
  0x00800000,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc00031,
  0x7f800071};
)";
    if (PageProbe)
      Text += pageAllocation();
    if (GSProbe)
      Text += "\n#include <intrin.h>\n";
    Text += R"(
int main(void) {
  unsigned saved = _mm_getcsr(), comparisons = 0;
  /* page allocation is inserted below for boundary probes. */
  for (unsigned env=0;env<32;++env)
    for (unsigned a=0;a<sizeof(inputs)/sizeof(inputs[0]);++a)
      for (unsigned b=0;b<sizeof(inputs)/sizeof(inputs[0]);++b) {
        unsigned char expected[160],actual[160];
        memset(expected,0xa5,sizeof(expected));
        memcpy(expected,&inputs[a],sizeof(inputs[0]));
        memcpy(expected+16,&inputs[b],sizeof(inputs[0]));
        unsigned state=0x1f80|((env&3)<<13)|((env&4)?0x40:0)|
                       ((env&8)?0x8000:0)|((env&16)?0x25:0);
        memcpy(expected+32,&state,4);
        memcpy(actual,expected,sizeof(actual));
        native_probe((uintptr_t)expected);
        _mm_setcsr(saved);
        isa_probe((uintptr_t)actual);
        _mm_setcsr(saved);
        if (memcmp(actual,expected,sizeof(actual))) {
          printf("env=%u a=%u b=%u\n",env,a,b);
          for (unsigned i=36;i<46;++i)
            printf("%u expected=%02x actual=%02x\n",i,expected[i],actual[i]);
          return 1;
        }
        ++comparisons;
      }
  printf("%u exact flag/MXCSR comparisons\n",comparisons);
  return 0;
}
)";
    if (PageProbe) {
      const std::string Start =
          "  /* page allocation is inserted below for boundary probes. */";
      Text.replace(Text.find(Start), Start.size(),
                   "  if(!allocate_pages()) return 77;");
      const std::string Marker =
          "        memcpy(actual,expected,sizeof(actual));";
      std::string Setup =
          "        uintptr_t "
          "address=(uintptr_t)(pages+page_size-sizeof(inputs[0]));\n"
          "        memcpy((void*)address,&inputs[b],sizeof(inputs[0]));\n";
      if (GSProbe)
        Setup += "        address -= __readgsqword(0x30); /* private buffer "
                 "relative to actual TEB/GS base */\n";
      Setup +=
          "        memcpy(expected+64,&address,sizeof(address));\n" + Marker;
      Text.replace(Text.find(Marker), Marker.size(), Setup);
    }
    return Text;
  }
};
class ComiForms : public ComiFixture,
                  public testing::WithParamInterface<ComiForm> {};
TEST_F(ComiFixture, DiscardedFlagsRetainComparisonAndMxcsrCompletion) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  for (const auto &F : {ComiForm{false, false, false, false},
                        ComiForm{true, true, true, true}}) {
    auto Bytes = comiBytes(F);
    const std::array<uint8_t, 2> Start{0x0f, 0x92};
    const std::array<uint8_t, 2> End{0x0f, 0xae};
    const auto First =
        std::search(Bytes.begin(), Bytes.end(), Start.begin(), Start.end());
    ASSERT_NE(First, Bytes.end());
    const auto Last = std::search(First, Bytes.end(), End.begin(), End.end());
    ASSERT_NE(Last, Bytes.end());
    Bytes.erase(First, Last);
    compareFloating(Bytes, F.Double);
  }
}
class ComiMemoryForms : public ComiFixture,
                        public testing::WithParamInterface<ComiForm> {};
TEST_P(ComiForms, FlagsAndMxcsrMatchOriginalInstructions) {
  const auto &F = GetParam();
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX required";
  compareFloating(comiBytes(F), F.Double);
}

TEST_P(ComiForms, UnmaskedInvalidPublishesNeitherFlagsNorStateStores) {
#if !defined(_WIN32) && !defined(__linux__)
  GTEST_SKIP() << "native fault context observer requires Windows/Linux x64";
#else
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  FaultMode = 0;
  compareFloating(comiBytes(GetParam()), GetParam().Double, true);
#endif
}
TEST_P(ComiForms, UnmaskedDenormalPublishesNeitherFlagsNorStateStores) {
#if !defined(_WIN32) && !defined(__linux__)
  GTEST_SKIP() << "native fault context observer requires Windows/Linux x64";
#else
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  FaultMode = 1;
  compareFloating(comiBytes(GetParam()), GetParam().Double, true);
#endif
}

TEST_P(ComiMemoryForms, MemoryReadsExactlyOneScalarAtProtectedPageEnd) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  PageProbe = true;
  compareFloating(comiBytes(GetParam(), true), GetParam().Double);
}
TEST_P(ComiMemoryForms,
       PartialScalarMemoryFaultPrecedesFlagsAndStatusCompletion) {
#if !defined(_WIN32) && !defined(__linux__)
  GTEST_SKIP() << "native fault context observer requires Windows/Linux x64";
#else
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  FaultMode = 2;
  compareFloating(comiBytes(GetParam(), true), GetParam().Double, true);
#endif
}
INSTANTIATE_TEST_SUITE_P(AllForms, ComiForms, testing::ValuesIn(comiForms()),
                         [](const auto &Info) { return comiName(Info.param); });
std::vector<ComiForm> memoryForms() {
  auto Forms = comiForms();
  std::erase_if(Forms, [](const auto &F) { return !F.Memory; });
  return Forms;
}
TEST(ComiContract, EveryPhysicalXmmRegisterHasAuthenticatedSseOrVexRoles) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  for (unsigned Control = 0; Control < 8; ++Control)
    for (unsigned Left = 0; Left < 16; ++Left)
      for (unsigned Right = 0; Right < 16; ++Right) {
        std::vector<uint8_t> Bytes;
        if (Control & 4)
          Bytes = {0xc4,
                   uint8_t(0xe1 ^ ((Left & 8) ? 0x80 : 0) ^
                           ((Right & 8) ? 0x20 : 0)),
                   uint8_t(Control & 2 ? 0xf9 : 0xf8)};
        else {
          if (Control & 2)
            Bytes.push_back(0x66);
          if (Left >= 8 || Right >= 8)
            Bytes.push_back(0x40 | ((Left & 8) ? 4 : 0) |
                            ((Right & 8) ? 1 : 0));
          Bytes.push_back(0x0f);
        }
        Bytes.push_back(Control & 1 ? 0x2e : 0x2f);
        Bytes.push_back(0xc0 | ((Left & 7) << 3) | (Right & 7));
        DecodedInsn Insn;
        ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Insn),
                  int(Bytes.size()));
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
        unsigned Found = 0;
        for (const auto &Op : Ops)
          if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
              Op.Inputs[0].Offset == unsigned(Intrinsic::X86FPComiState)) {
            ++Found;
            EXPECT_TRUE(x86FPStateShapeIsValid(
                Intrinsic::X86FPComiState, x86FPStateLowShape(Op, Arch::X64)));
          }
        ASSERT_EQ(Found, 1u);
      }
}
TEST_P(ComiMemoryForms, WindowsGsUsesActualBaseForPrivatePageEndSource) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << "native Windows x64 GS/TEB observer required";
#else
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX";
  PageProbe = true;
  GSProbe = true;
  compareFloating(comiBytes(GetParam(), true, true), GetParam().Double);
#endif
}
INSTANTIATE_TEST_SUITE_P(MemoryForms, ComiMemoryForms,
                         testing::ValuesIn(memoryForms()),
                         [](const auto &Info) { return comiName(Info.param); });
} // namespace
