//===- X64SvmContinuationExportTests.inc - Original VMRUN source --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Included by X64ContinuationExportTests.cpp inside its fixture namespace.

TEST_F(X64ContinuationExportTest,
       VmrunTransportsOriginalStateAndStopsAtVmexit) {
  load({0x48, 0x89, 0xf8,             // mov rax,rdi
        0xb9, 0x34, 0x12, 0x00, 0x00, // mov ecx,0x1234
        0x0f, 0x01, 0xd8,             // vmrun
        0x48, 0x89, 0xc8, 0xc3, 0x90, // mov rax,rcx; ret
        0x67, 0x0f, 0x01, 0xd8, 0xc3, // addr32 vmrun; ret
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x0f, 0x01, 0xd8, 0xc3}); // vmrun; ret
  const std::string Request = R"JSON({"profile":"amd-svm-v10",
    "mxcsr_mask":"0xffff", "follow_internal_calls":true,
    "instructions":[4096,4099,4104,4107,4110,4112,4116,4128,4131]})JSON";
  const char Provider[] = R"C(
#include <stdint.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) abort(); } while (0)
static unsigned calls, width_seen, initialize_simd;
static void svm_complete(void *opaque, unsigned width) {
    uint64_t *c=opaque; uint8_t *x=(void *)(uintptr_t)c[1];
    ++calls; width_seen=width;
    CHECK(c[3]==7);
    for(unsigned i=0;i<14;++i)c[4+i]+=i+1;
    c[5]=0x5678;
    for(unsigned i=0;i<16;++i)for(unsigned lane=0;lane<4;++lane) {
        unsigned off=(lane<2?160:576)+16*i+(lane%2)*8;
        uint64_t *v=(void *)(x+off); *v+=0x1234;
    }
    *(uint64_t *)(x+512)=initialize_simd?1:7;
    *(uint32_t *)(x+24)=initialize_simd?0x1f80:0x3f80;
    *(uint64_t *)(x+32)=0x8000000000000000ULL;
}

#define __builtin_ia32_vmrun_context svm_complete
#define NEVERD_X86_SYSTEM_REGISTER_NATIVE(native, ...) ((uint64_t)0xfeed1234)
)C";
  const char Harness[] = R"C(
static uint8_t guest[832] __attribute__((aligned(64)));
static uint8_t host[832] __attribute__((aligned(64)));
static neverd_x64_state_v10 make_state(uint64_t pc, uint64_t *stack) {
    neverd_x64_state_v10 s={0};
    for(unsigned i=0;i<16;++i)s.GPR[i]=0xabcdef0000000000ULL+i*37;
    for(unsigned i=0;i<sizeof(s.ZMM);++i)((uint8_t *)s.ZMM)[i]=(uint8_t)(i*13+1);
    s.GPR[0]=0xfedcba9800010000ULL; s.GPR[4]=(uint64_t)stack;
    s.GPR[7]=s.GPR[0]; s.RFlags=0x246; s.PC=pc; s.CR8=3; s.MXCSR=0x1f80;
    s.SVM.GuestXState=(uint64_t)guest; s.SVM.HostXState=(uint64_t)host;
    memset(guest,0,sizeof(guest));memset(host,0,sizeof(host));
    *(uint64_t *)(guest+512)=1;
    return s;
}
int main(void) {
    const unsigned map[14]={3,1,2,6,7,5,8,9,10,11,12,13,14,15};
    for(unsigned mode=0;mode<2;++mode)for(unsigned form=0;form<3;++form) {
        uint64_t stack[2]={0x7000,0xfeed};
        uint64_t pc=form==0?0x1000:form==1?0x1010:0x1020, budget=20;
        neverd_x64_state_v10 s=make_state(pc,stack), before=s;
        initialize_simd=mode; calls=0;
        CHECK(neverd_x64_continue_v10(&s,&budget)==13);
        CHECK(calls==1 && width_seen==(form==1?32:64));
        CHECK(budget==(form==0?17:19));
        CHECK(s.PC==(form==0?0x100b:form==1?0x1014:0x1023));
        CHECK(s.GPR[0]==before.GPR[0] && s.GPR[4]==before.GPR[4]);
        CHECK(s.RFlags==before.RFlags && s.CR2==0xfeed1234);
        CHECK(s.SVM.VMCB==before.GPR[0]);
        for(unsigned i=0;i<14;++i)
            CHECK(s.GPR[map[i]]==(i==1?0x5678:before.GPR[map[i]]+i+1));
        for(unsigned i=0;i<32;++i)for(unsigned lane=0;lane<8;++lane) {
            uint64_t old, now;
            memcpy(&old,before.ZMM[i]+lane*8,8);
            memcpy(&now,s.ZMM[i]+lane*8,8);
            CHECK(now==(i<16 && lane<4?(mode?0:old+0x1234):old));
        }
        CHECK(s.MXCSR==(mode?0x1f80:0x3f80));
        CHECK(*(uint64_t *)(guest+32)==0x8000000000000000ULL);
        CHECK(neverd_x64_continue_v10(&s,&budget)==3);
        CHECK(s.PC==0x7000 && s.GPR[4]==(uint64_t)(stack+1));
        CHECK(s.GPR[0]==(form==0?0x5678:before.GPR[0]));
    }
    for(unsigned failure=0;failure<11;++failure) {
        uint64_t stack[2]={0},budget=1;
        neverd_x64_state_v10 s=make_state(0x1020,stack);
        if(failure==0)s.SVM.GuestXState=0;
        if(failure==1)++s.SVM.HostXState;
        if(failure==2)s.SVM.HostXState=s.SVM.GuestXState;
        if(failure==3)*(uint64_t *)(guest+520)=1ULL<<63;
        if(failure==4)*(uint64_t *)(host+568)=1;
        if(failure==5)*(uint64_t *)(guest+512)=8;
        if(failure==6)s.RFlags|=0x100;
        if(failure==7)*(uint64_t *)(host+512)=8;
        if(failure==8)s.SVM.HostXState=s.SVM.GuestXState+64;
        if(failure==9)s.SVM.GuestXState=UINT64_MAX-63;
        if(failure==10)s.SVM.GuestXState=((uintptr_t)&s+63)&~(uintptr_t)63;
        neverd_x64_state_v10 before=s;
        uint8_t prior[832];memcpy(prior,guest,832);calls=0;
        CHECK(neverd_x64_continue_v10(&s,&budget)==5);
        CHECK(calls==0 && budget==1 && memcmp(&s,&before,sizeof(s))==0);
        CHECK(memcmp(prior,guest,832)==0);
    }
    return 0;
}
)C";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    auto [Code, Manifest] = source(Request, LLVM, 10);
    ASSERT_FALSE(Code.empty());
    EXPECT_NE(Code.find("__builtin_ia32_vmrun_context"), std::string::npos);
    EXPECT_NE(Manifest.find("\"native_requests\": 0"), std::string::npos);
    if (const char *Directory =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Directory);
      std::ofstream(fs::path(Directory) / (LLVM ? "svm-continuation-llvm.c"
                                                : "svm-continuation-high.c"))
          << Code;
      std::ofstream(fs::path(Directory) / (LLVM ? "svm-continuation-llvm.json"
                                                : "svm-continuation-high.json"))
          << Manifest;
    }
    {
      SCOPED_TRACE("instruction selection / switch dispatch");
      execute(std::string(Provider) + Code, Harness);
    }
    auto DirectRequest = Request;
    DirectRequest.insert(1, "\"dispatch\":\"direct\",");
    auto [Direct, DirectManifest] = source(DirectRequest, LLVM, 10);
    {
      SCOPED_TRACE("instruction selection / direct dispatch");
      execute(std::string(Provider) + Direct, Harness);
    }

    // Entry selection must retain instructions beyond ordinary strict-lift
    // refusal, so the user can select this original function as a region.
    auto [Selected, SelectedManifest] = source(
        R"JSON({"profile":"amd-svm-v10","mxcsr_mask":"0xffff",
                 "entries":[4096,4112,4128],"follow_internal_calls":true})JSON",
        LLVM, 10);
    EXPECT_EQ(Selected, Code);
    EXPECT_NE(SelectedManifest.find("vmexit_successors"), std::string::npos);
    const auto Input = tmpFile("svm-request.json");
    const auto Output = tmpFile("svm-output.c");
    const auto Report = tmpFile("svm-manifest.json");
    std::ofstream(Input) << Request;
    std::vector<std::string> Args{"decompile",
                                  tmpFile("input.elf").string(),
                                  "--x64-continuation=" + Input.string(),
                                  "-o",
                                  Output.string(),
                                  "--continuation-report=" + Report.string()};
    if (LLVM)
      Args.push_back("--llvm");
    const auto Ran = exec(ndBin(), Args);
    ASSERT_TRUE(Ran.ok()) << Ran.err;
    EXPECT_EQ(readOutput(Output), Code);
    EXPECT_EQ(readOutput(Report), Manifest);
  }
}

TEST_F(X64ContinuationExportTest, VmexitSelectionRetainsCallsAtTheirOriginalDepth) {
  std::vector<uint8_t> Code(0x34, 0x90);
  const uint8_t Caller[] = {0xe8, 0x1b, 0, 0, 0, 0x0f, 0x01, 0xd8,
                           0xe8, 0x23, 0, 0, 0, 0x0f, 0x01, 0xd8, 0xc3};
  const uint8_t First[] = {0x83, 0xc1, 1, 0xc3};
  const uint8_t Second[] = {0x83, 0xc1, 2, 0xc3};
  std::copy(std::begin(Caller), std::end(Caller), Code.begin());
  std::copy(std::begin(First), std::end(First), Code.begin() + 0x20);
  std::copy(std::begin(Second), std::end(Second), Code.begin() + 0x30);
  load(Code);
  const char Provider[] = R"C(
#include <stdint.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) abort(); } while (0)
static unsigned calls;
static void svm_complete(void *opaque, unsigned width) {
    CHECK(width==64); ++calls; ((uint64_t *)opaque)[5]+=10;
}
#define __builtin_ia32_vmrun_context svm_complete
#define NEVERD_X86_SYSTEM_REGISTER_NATIVE(native, ...) ((uint64_t)0)
)C";
  for (unsigned Depth : {0u, 1u})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Depth);
      SCOPED_TRACE(LLVM);
      const auto Request =
          R"({"profile":"amd-svm-v10","mxcsr_mask":"0xffff","entries":[4096],"follow_internal_calls":true,"dispatch":"direct","callee_depth":)" +
          std::to_string(Depth) + "}";
      auto [Source, Manifest] = source(Request, LLVM, 10);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed));
      const auto *Regions = Parsed->getAsObject()->getArray("regions");
      ASSERT_NE(Regions, nullptr);
      EXPECT_EQ(Regions->size(), Depth ? 5u : 3u);
      for (size_t I = 0; I != Regions->size(); ++I)
        EXPECT_EQ((*Regions)[I].getAsObject()->getInteger("callee_depth"),
                  I < 3 ? 0 : 1);
      execute(std::string(Provider) + Source,
              "#define DEPTH " + std::to_string(Depth) + R"C(
static uint8_t guest[832] __attribute__((aligned(64)));
static uint8_t host[832] __attribute__((aligned(64)));
int main(void) {
    uint64_t stack[4]={0,0x7000,0,0},budget=10;
    neverd_x64_state_v10 s={0};
    s.PC=0x1000;s.RFlags=2;s.MXCSR=0x1f80;s.GPR[4]=(uintptr_t)(stack+1);
    s.SVM.GuestXState=(uintptr_t)guest;s.SVM.HostXState=(uintptr_t)host;
    uint64_t status=neverd_x64_continue_v10(&s,&budget);
    CHECK(stack[0]==0x1005);
    if (!DEPTH) {
        CHECK(status==1 && budget==9 && s.PC==0x1020 && !calls);
        CHECK(s.GPR[4]==(uintptr_t)stack && !s.GPR[1]);
    } else {
        CHECK(status==13 && budget==6 && s.PC==0x1008 && calls==1);
        CHECK(s.GPR[4]==(uintptr_t)(stack+1) && s.GPR[1]==11);
        CHECK(neverd_x64_continue_v10(&s,&budget)==13);
        CHECK(stack[0]==0x100d && budget==2 && s.PC==0x1010 && calls==2);
        CHECK(s.GPR[4]==(uintptr_t)(stack+1) && s.GPR[1]==23);
        CHECK(neverd_x64_continue_v10(&s,&budget)==3);
        CHECK(s.PC==0x7000 && budget==1 && s.GPR[4]==(uintptr_t)(stack+2));
    }
    return 0;
}
)C");
    }
}
