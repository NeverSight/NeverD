//===- X64SmmContinuationExportTests.inc - Native RSM source -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Included by X64ContinuationExportTests.cpp inside its fixture namespace.

TEST_F(X64ContinuationExportTest, SmmReturnImportsSoftwareOwnedState) {
  load({0x0f, 0xaa, 0x90, 0x66, 0x0f, 0xaa, 0xc3});
  const char Request[] = R"JSON({"profile":"native-smm-v12",
    "mxcsr_mask":"0xffff", "segment_base_msr_address_bits":48,
    "instructions":[4096]})JSON";
  const char Provider[] = R"C(
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
static jmp_buf resume;
static unsigned calls;
static const uint64_t *observed;
__attribute__((noreturn)) static void smm_transfer(const void *opaque) {
    ++calls; observed = opaque; longjmp(resume, 1);
}
#define __builtin_ia32_rsm_context smm_transfer
#define CHECK(c) do { if (!(c)) abort(); } while (0)
)C";
  const char Harness[] = R"C(
static uint8_t image[832] __attribute__((aligned(64)));
static neverd_x64_state_v12 state __attribute__((aligned(64)));
static void setup(void) {
    memset(&state, 0x5a, sizeof(state)); memset(image,0,sizeof(image));
    state.PC=0x1000; state.RFlags=0x246; state.CR8=13;
    state.CR2=0x123456789abcdef0ULL; state.KernelGSBase=0xffff800012345678ULL;
    state.MXCSR=0x5f80; state.SMM.XState=(uintptr_t)image;
    for(unsigned i=0;i<sizeof(state.ZMM);++i)((uint8_t *)state.ZMM)[i]=(uint8_t)(i*17+7);
    for(unsigned i=0;i<128;++i)image[32+i]=(uint8_t)(i*13);
    *(uint64_t *)(image+512)=1;
}
int main(void) {
    for(unsigned bv=0;bv<8;++bv) {
        setup(); *(uint64_t *)(image+512)=bv;
        neverd_x64_state_v12 before=state;
        uint8_t prior[832]; memcpy(prior,image,832);
        calls=0;
        if(!setjmp(resume)) { neverd_step_v12_1000((void *)&state); abort(); }
        CHECK(calls==1 && observed==(const void *)&state.SMM);
        CHECK(state.SMM.XState==(uintptr_t)image && state.SMM.XStateMask==7);
        CHECK(state.SMM.CR2==state.CR2 && state.SMM.CR8==state.CR8);
        CHECK(state.SMM.KernelGSBase==state.KernelGSBase);
        CHECK(!memcmp(&state,&before,offsetof(neverd_x64_state_v12,SMM)));
        CHECK(!memcmp(image+32,prior+32,128));
        CHECK(*(uint64_t *)(image+512)==(bv|6));
        CHECK(*(uint32_t *)(image+24)==state.MXCSR);
        for(unsigned i=0;i<16;++i) {
            CHECK(!memcmp(image+160+i*16,state.ZMM[i],16));
            CHECK(!memcmp(image+576+i*16,state.ZMM[i]+16,16));
        }
    }
    for(unsigned invalid=0;invalid<10;++invalid) {
        setup(); calls=0;
        if(invalid==0)state.SMM.XState=0;
        if(invalid==1)++state.SMM.XState;
        if(invalid==2)state.SMM.XState=UINT64_MAX-63;
        if(invalid==3) {
            state.SMM.XState=(uintptr_t)&state;
            memset((uint8_t *)&state+512,0,64);
        }
        if(invalid==4)*(uint64_t *)(image+512)=8;
        if(invalid==5)*(uint64_t *)(image+520)=1;
        if(invalid==6)image[575]=1;
        if(invalid==7)state.KernelGSBase=0x800000000000ULL;
        if(invalid==8)state.CR8=16;
        if(invalid==9)state.MXCSR=0x10000;
        neverd_x64_state_v12 before=state;
        uint8_t prior[832];memcpy(prior,image,832);
        CHECK(neverd_step_v12_1000((void *)&state)==5);
        CHECK(calls==0 && !memcmp(&state,&before,sizeof(state)));
        CHECK(!memcmp(image,prior,832));
    }
    return 0;
}
)C";
  for (bool LLVM : {false, true}) {
    auto [Code, Manifest] = source(Request, LLVM, 12);
    ASSERT_FALSE(Code.empty());
    EXPECT_NE(Code.find("__builtin_ia32_rsm_context"), std::string::npos);
    EXPECT_EQ(Code.find("__asm__"), std::string::npos);
    EXPECT_NE(Manifest.find("\"native_requests\": 0"), std::string::npos);
    execute(std::string(Provider) + Code, Harness);
    auto Selection = std::string(Request);
    Selection.replace(Selection.find("instructions"), 12, "entries");
    auto [Selected, SelectedManifest] = source(Selection, LLVM, 12);
    EXPECT_EQ(Selected, Code);
    if (const char *Directory =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Directory);
      std::ofstream(fs::path(Directory) / (LLVM ? "smm-llvm.c" : "smm-high.c"))
          << Code;
      std::ofstream(fs::path(Directory) /
                    (LLVM ? "smm-llvm.json" : "smm-high.json"))
          << Manifest;
    }
  }
}

TEST_F(X64ContinuationExportTest, SmmReturnRequiresItsBoundEnvironment) {
  load({0x0f, 0xaa, 0x90, 0x66, 0x0f, 0xaa, 0xc3});
  for (
      const char *Request :
      {R"({"profile":"native-smm-v12","mxcsr_mask":"0xffff","instructions":[4096]})",
       R"({"profile":"native-smm-v12","mxcsr_mask":"0xffff","segment_base_msr_address_bits":32,"instructions":[4096]})"}) {
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
        R"({"profile":"native-smm-v12","mxcsr_mask":"0xffff","segment_base_msr_address_bits":48,"instructions":[4098,4099]})",
        LLVM, 12);
    EXPECT_EQ(Code.find("__builtin_ia32_rsm_context"), std::string::npos);
    EXPECT_NE(Manifest.find("\"native_requests\": 2"), std::string::npos);
    execute(Code, R"C(
#include <stdlib.h>
int main(void) {
    neverd_x64_state_v12 state={0};
    state.RFlags=0x202;state.MXCSR=0x1f80;state.SMM.XState=1;
    state.PC=0x1002;neverd_x64_state_v12 before=state;
    if(neverd_step_v12_1002((void *)&state)!=4 ||
       memcmp(&state,&before,sizeof(state)))abort();
    state.PC=0x1003;before=state;
    if(neverd_step_v12_1003((void *)&state)!=4 ||
       memcmp(&state,&before,sizeof(state)))abort();
    return 0;
}
)C");
  }
}
