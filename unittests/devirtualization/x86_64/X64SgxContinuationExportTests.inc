//===- X64SgxContinuationExportTests.inc - Original-site SGX source ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Included by X64ContinuationExportTests.cpp inside its fixture namespace.

TEST_F(X64ContinuationExportTest,
       SgxTransferPreservesOriginalSiteAndInputState) {
  load({0x0f, 0x01, 0xd7, 0x90, 0xc3});
  const char Request[] = R"JSON({"profile":"native-sgx-v13",
    "mxcsr_mask":"0xffff","instructions":[4096]})JSON";
  const char Provider[] = R"C(
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
static jmp_buf resume;
static unsigned calls;
static const uint64_t *observed;
static uint64_t site;
__attribute__((noreturn)) static void sgx_transfer(const void *opaque,
                                                 unsigned long long pc) {
    ++calls; observed=opaque; site=pc; longjmp(resume,1);
}
#define __builtin_ia32_enclu_transfer_at_context sgx_transfer
#define CHECK(c) do { if (!(c)) abort(); } while (0)
)C";
  const char Harness[] = R"C(
static uint8_t image[832] __attribute__((aligned(64)));
static neverd_x64_state_v13 state;
static void setup(void) {
    memset(&state,0x5a,sizeof(state)); memset(image,0,sizeof(image));
    state.PC=0x1000; state.RFlags=0x240ed7; state.CR8=13;
    state.MXCSR=0x5f80; state.SGX.XState=(uintptr_t)image;
    for(unsigned i=0;i<16;++i)state.GPR[i]=0x1234567800000000ULL+i*0x1234;
    state.GPR[0]=0xcafebabe00000002ULL;
    for(unsigned i=0;i<sizeof(state.ZMM);++i)((uint8_t *)state.ZMM)[i]=(uint8_t)(i*17+7);
    for(unsigned i=0;i<128;++i)image[32+i]=(uint8_t)(i*13);
    *(uint64_t *)(image+512)=1;
}
int main(void) {
    for(unsigned leaf=2;leaf<=4;++leaf)for(unsigned bv=0;bv<8;++bv) {
        setup(); state.GPR[0]=0xcafebabe00000000ULL|leaf;
        *(uint64_t *)(image+512)=bv;
        neverd_x64_state_v13 before=state;
        uint8_t prior[832]; memcpy(prior,image,832);
        calls=0;
        if(!setjmp(resume)) { neverd_step_v13_1000((void *)&state); abort(); }
        CHECK(calls==1 && observed==(const void *)&state.SGX && site==0x1000);
        CHECK(state.SGX.XState==(uintptr_t)image && state.SGX.RFlags==state.RFlags);
        CHECK(!memcmp(state.SGX.GPR,state.GPR,sizeof(state.GPR)));
        CHECK(!memcmp(&state,&before,offsetof(neverd_x64_state_v13,SGX)));
        CHECK(!memcmp(image+32,prior+32,128));
        CHECK(*(uint64_t *)(image+512)==(bv|6));
        CHECK(*(uint32_t *)(image+24)==state.MXCSR);
        for(unsigned i=0;i<16;++i) {
            CHECK(!memcmp(image+160+i*16,state.ZMM[i],16));
            CHECK(!memcmp(image+576+i*16,state.ZMM[i]+16,16));
        }
    }
    for(unsigned invalid=0;invalid<24;++invalid) {
        setup(); calls=0;
        if(invalid==0)state.SGX.XState=0;
        if(invalid==1)++state.SGX.XState;
        if(invalid==2)state.SGX.XState=UINT64_MAX-63;
        if(invalid==3)state.SGX.XState=(uintptr_t)&state;
        if(invalid==4)*(uint64_t *)(image+512)=8;
        if(invalid==5)*(uint64_t *)(image+520)=1;
        if(invalid==6)image[575]=1;
        if(invalid==7)state.PC++;
        if(invalid==8)state.CR8=16;
        if(invalid==9)state.MXCSR=0x10000;
        if(invalid==10)state.RFlags&=~0x200ULL;
        if(invalid>=11 && invalid<=17) {
            const unsigned bits[]={8,12,14,16,17,19,20};
            state.RFlags|=1ULL<<bits[invalid-11];
        }
        if(invalid>=18) {
            const uint32_t leaves[]={0,1,5,11,0xffffffff,0x80000002};
            state.GPR[0]=0xcafebabe00000000ULL|leaves[invalid-18];
            state.SGX.XState=1; // Leaf rejection must precede image access.
        }
        neverd_x64_state_v13 before=state;
        uint8_t prior[832];memcpy(prior,image,832);
        CHECK(neverd_step_v13_1000((void *)&state)==5);
        CHECK(calls==0 && !memcmp(&state,&before,sizeof(state)));
        CHECK(!memcmp(image,prior,832));
    }
    return 0;
}
)C";
  for (bool LLVM : {false, true}) {
    auto [Code, Manifest] = source(Request, LLVM, 13);
    ASSERT_FALSE(Code.empty());
    EXPECT_NE(Code.find("__builtin_ia32_enclu_transfer_at_context"),
              std::string::npos);
    EXPECT_EQ(Code.find("__asm__"), std::string::npos);
    EXPECT_NE(Manifest.find("\"native_requests\": 0"), std::string::npos);
    execute(std::string(Provider) + Code, Harness);
    auto Selection = std::string(Request);
    Selection.replace(Selection.find("instructions"), 12, "entries");
    auto [Selected, SelectedManifest] = source(Selection, LLVM, 13);
    EXPECT_EQ(Selected, Code);
    if (const char *Directory =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Directory);
      std::ofstream(fs::path(Directory) / (LLVM ? "sgx-llvm.c" : "sgx-high.c"))
          << Code;
      std::ofstream(fs::path(Directory) /
                    (LLVM ? "sgx-llvm.json" : "sgx-high.json"))
          << Manifest;
    }
  }
}

TEST_F(X64ContinuationExportTest,
       SgxTransferRefusesUnboundOrDifferentInstructions) {
  load({0x0f, 0x01, 0xd7, 0x90, 0x66, 0x0f, 0x01, 0xd7, 0xc3});
  for (
      const char *Request :
      {R"({"profile":"native-sgx-v13","instructions":[4096]})",
       R"({"profile":"native-sgx-v13","mxcsr_mask":"0xffff","paging_address_bits":48,"instructions":[4096]})",
       R"({"profile":"native-sgx-v13","mxcsr_mask":"0xffff","event_delivery":true,"instructions":[4096]})"}) {
    const char *Report = nullptr;
    EXPECT_EQ(neverd_x64_continuation_source_v1(
                  Session, Request, std::char_traits<char>::length(Request), 0,
                  &Report),
              nullptr);
    EXPECT_EQ(Report, nullptr);
    EXPECT_FALSE(take(neverd_last_error(Session)).empty());
  }
  for (bool LLVM : {false, true}) {
    auto [Code, Manifest] = source(
        R"({"profile":"native-sgx-v13","mxcsr_mask":"0xffff","instructions":[4099,4100]})",
        LLVM, 13);
    EXPECT_EQ(Code.find("__builtin_ia32_enclu_transfer_at_context"),
              std::string::npos);
    EXPECT_NE(Manifest.find("\"native_requests\": 2"), std::string::npos);
  }
}
