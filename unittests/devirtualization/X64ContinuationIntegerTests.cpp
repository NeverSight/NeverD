//===- X64ContinuationIntegerTests.cpp - Native integer state C --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare state C with original vector, multiply and atomic instructions.
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/X64ContinuationExport.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {
using namespace neverd;
using Bytes = std::vector<uint8_t>;

constexpr char StateSeed[] = R"C(
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) abort(); } while (0)
static neverd_x64_state_v6 seed(uint64_t pc) {
  neverd_x64_state_v6 s={0};s.PC=pc;s.RFlags=0x246;s.MXCSR=0x1f80;
  for(unsigned i=0;i<16;++i)s.GPR[i]=0xfedcba9876540000ULL+i*73;
  for(unsigned i=0;i<sizeof(s.ZMM);++i)((uint8_t*)s.ZMM)[i]=i*13+7;
  s.CS=(neverd_x64_segment){0,0xffffffff,8,0xa09b};
  s.SS=(neverd_x64_segment){0,0xffffffff,16,0xc093};
  s.DS=s.ES=s.SS;return s;
}
)C";

std::string original(unsigned Index, const Bytes &Instruction,
                     const std::string &Arguments, const std::string &Before,
                     const std::string &After) {
  const std::string Name = "original_" + std::to_string(Index);
  std::string Source = "extern uint64_t " + Name + Arguments + ";\n";
  Source += "__asm__(\".text\\n.p2align 4\\n.fill 16,1,0x90\\n.globl " + Name +
            "\\n" + Name + ":\\n" + Before + ".byte ";
  for (size_t I = 0; I != Instruction.size(); ++I) {
    if (I)
      Source += ',';
    Source += std::to_string(Instruction[I]);
  }
  return Source + "\\n" + After + "ret\\n\");\n";
}

class X64ContinuationIntegerTest : public NeverDLiftTest {
protected:
  void run(const std::vector<Bytes> &Instructions, const std::string &Harness) {
#if !defined(__linux__) || !defined(__x86_64__)
    GTEST_SKIP() << "native instruction comparison requires Linux x64";
#else
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.Base = Image.Entry = 0x1000;
    Segment Code;
    Code.VA = Image.Entry;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    llvm::json::Array Addresses;
    std::string Sizes = "static const unsigned instruction_size[]={";
    for (size_t I = 0; I != Instructions.size(); ++I) {
      ASSERT_LE(Instructions[I].size(), 15u);
      Addresses.push_back(uint64_t(Image.Entry + I * 32));
      Code.Data.resize(I * 32, 0xcc);
      Code.Data.insert(Code.Data.end(), Instructions[I].begin(),
                       Instructions[I].end());
      Sizes += std::to_string(Instructions[I].size()) + ',';
    }
    Sizes += "};\n";
    Code.Size = Code.FileSz = Code.Data.size();
    Image.Segments.push_back(std::move(Code));
    llvm::json::Object Request{{"profile", "kernel-events-v6"},
                               {"mxcsr_mask", 0xffbf},
                               {"paging_address_bits", 48},
                               {"instructions", std::move(Addresses)}};
    const std::string JSON =
        llvm::formatv("{0}", llvm::json::Value(std::move(Request))).str();
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      auto Export = exportX64Continuation(Image, JSON, LLVM);
      ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
      auto Manifest = llvm::json::parse(Export->Manifest);
      ASSERT_TRUE(bool(Manifest));
      ASSERT_EQ(Manifest->getAsObject()->getInteger("native_requests"), 0)
          << Export->Manifest;
      const auto Source = tmpFile(LLVM ? "integer-llvm.c" : "integer-high.c");
      std::ofstream(Source) << Export->Source << StateSeed << Sizes << Harness;
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        const auto Executable = tmpFile(std::string("integer") + Optimization);
        const auto Built =
            exec(NEVERD_TEST_CLANG,
                 {"-std=gnu11", Optimization, "-Werror=uninitialized",
                  "-Werror=return-type", "-fsanitize=address,undefined",
                  Source.string(), "-o", Executable.string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
        const auto Ran = exec(Executable.string(), {});
        ASSERT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode;
        ASSERT_TRUE(Ran.err.empty()) << Ran.err;
      }
    }
#endif
  }
};

TEST_F(X64ContinuationIntegerTest, LegacyVectorsRetainWholeState) {
  std::vector<Bytes> Instructions{
      {0x0f, 0x57, 0xc1},       {0x66, 0x0f, 0x57, 0xc1},
      {0x0f, 0x57, 0xc0},       {0x66, 0x48, 0x0f, 0x7e, 0xc8},
      {0x66, 0x0f, 0x7e, 0xc8}, {0x66, 0x48, 0x0f, 0x6e, 0xc8},
      {0x66, 0x0f, 0x6e, 0xc8}, {0xf2, 0x0f, 0x10, 0x41, 0x10}};
  for (uint8_t Count : {0, 1, 15, 16, 255})
    for (uint8_t ModRM : {0xd9, 0xf9})
      Instructions.push_back({0x66, 0x0f, 0x73, ModRM, Count});
  std::string Harness, Calls;
  for (unsigned I = 0; I != Instructions.size(); ++I) {
    Harness +=
        original(I, Instructions[I], "(const uint8_t*,uint64_t,uint8_t*)",
                 "movdqu (%rdi),%xmm1\\nmovdqu 16(%rdi),%xmm0\\n"
                 "mov %rdi,%rcx\\nmov %rsi,%rax\\n",
                 "movdqu %xmm1,(%rdx)\\nmovdqu %xmm0,16(%rdx)\\n");
    Calls += "original_" + std::to_string(I) + ',';
  }
  Harness += "static uint64_t (*native[])(const uint8_t*,uint64_t,uint8_t*)={" +
             Calls + "};\n";
  Harness += R"C(
int main(void) {
  for(unsigned n=0;n<sizeof(native)/sizeof(*native);++n)for(unsigned v=0;v<16;++v) {
    uint8_t input[33],output[32];for(unsigned b=0;b<sizeof(input);++b)input[b]=b*71+v*13;
    neverd_x64_state_v6 state=seed(0x1000+n*32);
    memcpy(state.ZMM[1],input+1,16);memcpy(state.ZMM[0],input+17,16);
    state.GPR[0]=0x80000000deadbeefULL*(v+1);state.GPR[1]=(uintptr_t)(input+1);
    neverd_x64_state_v6 expected=state;
    expected.GPR[0]=native[n](input+1,state.GPR[0],output);
    memcpy(expected.ZMM[1],output,16);memcpy(expected.ZMM[0],output+16,16);
    expected.PC+=instruction_size[n];uint64_t budget=1;
    CHECK(neverd_x64_continue_v6(&state,&budget)==9 && !budget);
    CHECK(!memcmp(&state,&expected,sizeof(state)));
  }
  return 0;
}
)C";
  run(Instructions, Harness);
}

TEST_F(X64ContinuationIntegerTest, WideProductsAndArbitraryFlags) {
  const std::vector<Bytes> Instructions{
      {0x48, 0xf7, 0xe1}, {0x48, 0xf7, 0xe9}, {0x48, 0x0f, 0xaf, 0xc1}};
  std::string Harness, Calls;
  for (unsigned I = 0; I != Instructions.size(); ++I) {
    Harness += original(I, Instructions[I], "(uint64_t,uint64_t,uint64_t*)",
                        "mov %rdx,%r8\\nmov %rdi,%rax\\nmov %rsi,%rcx\\n",
                        "pushfq\\npop %r9\\nmov %rax,(%r8)\\n"
                        "mov %rdx,8(%r8)\\nmov %r9,16(%r8)\\n");
    Calls += "original_" + std::to_string(I) + ',';
  }
  Harness += "static uint64_t (*native[])(uint64_t,uint64_t,uint64_t*)={" +
             Calls + "};\n";
  Harness += R"C(
int main(void) {
  const uint64_t values[]={0,1,2,0x7fffffffffffffffULL,0x8000000000000000ULL,
                          UINT64_MAX,0x100000000ULL,0x80000000,0xfedcba9876543210ULL};
  const unsigned arbitrary[]={7,6,4,2};
  for(unsigned n=0;n<3;++n)for(unsigned a=0;a<9;++a)for(unsigned b=0;b<9;++b)
    for(unsigned pattern=0;pattern<16;++pattern) {
      uint64_t output[3];native[n](values[a],values[b],output);
      neverd_x64_state_v6 state=seed(0x1000+n*32);
      state.GPR[0]=values[a];state.GPR[1]=values[b];
      for(unsigned bit=0;bit<4;++bit)state.Undefined[bit]=(pattern>>bit)&1;
      neverd_x64_state_v6 expected=state;
      expected.GPR[0]=output[0];if(n<2)expected.GPR[2]=output[1];
      expected.RFlags=(state.RFlags&~0x8d5ULL)|(output[2]&0x801);
      for(unsigned bit=0;bit<4;++bit)expected.RFlags|=state.Undefined[bit]<<arbitrary[bit];
      expected.PC+=instruction_size[n];uint64_t budget=1;
      CHECK(neverd_x64_continue_v6(&state,&budget)==9 && !budget);
      CHECK(!memcmp(&state,&expected,sizeof(state)));
    }
  return 0;
}
)C";
  run(Instructions, Harness);
}

TEST_F(X64ContinuationIntegerTest, AtomicMemoryRetainsOldValuesAndFlags) {
  std::vector<Bytes> Instructions;
  std::string Harness, Calls, Kinds = "static const unsigned kind[]={";
  for (unsigned Kind = 0; Kind != 8; ++Kind)
    for (unsigned Width : {1u, 2u, 4u, 8u}) {
      if (Kind >= 5 && Width == 1)
        continue;
      Bytes Code;
      if (Kind != 0)
        Code.push_back(0xf0);
      if (Width == 2)
        Code.push_back(0x66);
      if (Width == 8 || Width == 1)
        Code.push_back(Width == 8 ? 0x48 : 0x40);
      if (Kind == 0)
        Code.insert(Code.end(), {uint8_t(Width == 1 ? 0x86 : 0x87), 0x37});
      else if (Kind == 1)
        Code.insert(Code.end(),
                    {0x0f, uint8_t(Width == 1 ? 0xc0 : 0xc1), 0x37});
      else if (Kind < 5) {
        const unsigned Opcode = Kind == 2 ? 0x20 : Kind == 3 ? 0x08 : 0x30;
        Code.insert(Code.end(), {uint8_t(Opcode + (Width != 1)), 0x37});
      } else {
        Code.insert(Code.end(), {0x0f,
                                 uint8_t(Kind == 5   ? 0xab
                                         : Kind == 6 ? 0xb3
                                                     : 0xbb),
                                 0x37});
      }
      const unsigned I = Instructions.size();
      Instructions.push_back(Code);
      Harness += original(I, Code, "(uint8_t*,uint64_t,uint64_t*,uint64_t)",
                          "push %rcx\\npopfq\\n",
                          "pushfq\\npop %rax\\nmov %rsi,(%rdx)\\n");
      Calls += "original_" + std::to_string(I) + ',';
      Kinds += std::to_string(Kind) + ',';
    }
  Harness += Kinds +
             "};\nstatic uint64_t "
             "(*native[])(uint8_t*,uint64_t,uint64_t*,uint64_t)={" +
             Calls + "};\n";
  Harness += R"C(
int main(void) {
  const uint64_t values[]={0,1,7,0x80,0x8000,0x80000000,0x8000000000000000ULL,UINT64_MAX};
  const int64_t indices[]={-129,-65,-17,-1,0,1,7,15,31,63,64,127};
  const unsigned bit_flags[]={11,7,4,2};
  for(unsigned n=0;n<sizeof(native)/sizeof(*native);++n)for(unsigned a=0;a<8;++a)
    for(unsigned b=0;b<(kind[n]>=5?12:8);++b)for(unsigned off=0;off<2;++off)
      for(unsigned pattern=0;pattern<16;++pattern) {
        if(kind[n]>=5 && indices[b]<0 && off)continue;
        _Alignas(64) uint8_t x[256],y[256];memset(x,(uint8_t)values[a],sizeof(x));
        memcpy(y,x,sizeof(x));uint64_t value=kind[n]>=5?(uint64_t)indices[b]:values[b],after;
        neverd_x64_state_v6 state=seed(0x1000+n*32);
        state.GPR[7]=(uintptr_t)(y+128+off);state.GPR[6]=value;
        for(unsigned bit=0;bit<4;++bit)state.Undefined[bit]=(pattern>>bit)&1;
        uint64_t flags=native[n](x+128+off,value,&after,state.RFlags);
        neverd_x64_state_v6 expected=state;expected.GPR[6]=after;expected.RFlags=flags;
        if(kind[n]>=2 && kind[n]<5)expected.RFlags=(flags&~16ULL)|(state.Undefined[0]<<4);
        if(kind[n]>=5)for(unsigned bit=0;bit<4;++bit)
          expected.RFlags=(expected.RFlags&~(1ULL<<bit_flags[bit]))|(state.Undefined[bit]<<bit_flags[bit]);
        expected.PC+=instruction_size[n];uint64_t budget=1;
        CHECK(neverd_x64_continue_v6(&state,&budget)==9 && !budget);
        CHECK(!memcmp(&state,&expected,sizeof(state)) && !memcmp(x,y,sizeof(x)));
      }
  return 0;
}
)C";
  run(Instructions, Harness);
}
} // namespace
