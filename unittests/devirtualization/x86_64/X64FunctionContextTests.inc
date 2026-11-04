//===- X64FunctionContextTests.inc - Static activation checks -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included after X64ContinuationExportTest and Seed. Separate compile-only
/// checks from explicit runtime instruction and activation regressions.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest, FunctionContextRejectsUnboundContracts) {
  load({0x90, 0xc3});
  for (
      const auto &Bad :
      {request(R"("entries":[4096],"function_context":1)"),
       request(R"("entries":[4096],"function_context":null)"),
       request(R"("entries":[4096],"function_context":true)"),
       request(
           R"("instructions":[4096],"function_context":true,"follow_internal_calls":true)"),
       request(
           R"("entries":[4096],"function_context":true,"follow_internal_calls":true,"paged_memory":true)"),
       request(
           R"("entries":[4096],"function_context":true,"follow_internal_calls":true,"scheduled_events":true)"),
       request(
           R"("entries":[4096],"function_context":true,"follow_internal_calls":true,"native_rtm_regions":true)"),
       request(
           R"("entries":[4096],"function_context":true,"follow_internal_calls":true,"native_rtm_abort_regions":true)"),
       std::string(
           R"({"profile":"intel-fast-system-v7","mxcsr_mask":"0xffbf","paging_address_bits":48,"sysenter_msr_address_bits":48,"entries":[4096],"function_context":true,"follow_internal_calls":true})")}) {
    SCOPED_TRACE(Bad);
    const char *Report = "old";
    EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Bad.data(), Bad.size(),
                                                0, &Report),
              nullptr);
    EXPECT_EQ(Report, nullptr);
    EXPECT_FALSE(take(neverd_last_error(Session)).empty());
  }
  for (bool LLVM : {false, true}) {
    const auto Base = source(request(R"("entries":[4096])"), LLVM);
    const auto Disabled =
        source(request(R"("entries":[4096],"function_context":false)"), LLVM);
    EXPECT_EQ(Base.first, Disabled.first);
    EXPECT_EQ(Base.first.find("neverd_x64_function_context"),
              std::string::npos);
  }
}

class X64FunctionContextTest : public X64ContinuationExportTest {
protected:
  void checkFunctionContext(bool Run) {
    std::vector<uint8_t> Code(0x32, 0x90);
    const std::vector<uint8_t> Root = {0x48, 0x83, 0xec, 0x28,    // sub rsp,40
                                       0xe8, 0x17, 0,    0,    0, // call 0x1020
                                       0x48, 0x83, 0xc4, 0x28,    // add rsp,40
                                       0xc2, 0x10, 0};            // ret 16
    const std::vector<uint8_t> Callee = {0x8b, 0x44, 0x24, 0x2c,
                                         0xc3}; // mov eax,[rsp+44]; ret
    std::copy(Root.begin(), Root.end(), Code.begin());
    Code[0x10] = 0xff;
    Code[0x11] = 0xc0;
    Code[0x12] = 0xc3; // inc eax; ret
    std::copy(Callee.begin(), Callee.end(), Code.begin() + 0x20);
    Code[0x30] = 0x0f;
    Code[0x31] = 0x0b; // Unbound UD2.
    load(Code, {{0x1000, 16}, {0x1010, 3}, {0x1020, 5}, {0x1030, 2}});
    const char Probes[] = R"C(
static int declined(neverd_x64_state_v6 *s,uint64_t boundary,void *user) {
    ++*(unsigned *)user;return 0;
}
#define PREPARE() \
    uint64_t stack[16]={0},budget=8,env_budget=4;unsigned attempts=0; \
    neverd_x64_state_v6 s=seed(); \
    neverd_x64_function_context_v1 f={0}; \
    s.GPR[4]=(uintptr_t)&stack[8];stack[8]=0x1010; \
    neverd_x64_begin_function_v6(&s,&f)
int proof_return(uint64_t incoming) {
    PREPARE();stack[7]=incoming;
    uint64_t status=neverd_x64_run_function_v6(&s,&budget,&env_budget,&f,declined,&attempts);
    return status!=3 || f.Phase!=NEVERD_X64_FUNCTION_RETURNED || budget!=2 ||
        env_budget!=4 || attempts!=0 || s.PC!=0x1010 ||
        s.GPR[0]!=(uint32_t)(incoming>>32) || s.GPR[4]!=(uintptr_t)&stack[11];
}
int proof_pause(uint64_t incoming) {
    PREPARE();stack[7]=incoming;budget=4;
    uint64_t status=neverd_x64_continue_function_v6(&s,&budget,&f);
    int bad=status!=9 || budget!=0 || f.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        s.PC!=0x1009 || s.GPR[4]!=(uintptr_t)&stack[3];
    budget=2;status=neverd_x64_continue_function_v6(&s,&budget,&f);
    return bad || status!=3 || budget!=0 || f.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        s.PC!=0x1010 || s.GPR[0]!=(uint32_t)(incoming>>32);
}
int proof_changed_return(void) {
    PREPARE();stack[8]=0x7777;
    uint64_t status=neverd_x64_continue_function_v6(&s,&budget,&f);
    return status!=3 || f.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        s.PC!=0x7777 || budget!=2;
}
int proof_pivot(void) {
    PREPARE();s.PC=0x100d;s.GPR[4]=(uintptr_t)&stack[5];stack[5]=0x7777;budget=1;
    uint64_t status=neverd_x64_continue_function_v6(&s,&budget,&f);
    return status!=3 || budget!=0 || f.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        s.PC!=0x7777 || s.GPR[4]!=f.ReturnSlot;
}
int proof_last_instruction(void) {
    PREPARE();s.PC=0x100d;budget=1;
    uint64_t status=neverd_x64_continue_function_v6(&s,&budget,&f);
    return status!=3 || budget!=0 || f.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        s.PC!=0x1010;
}
int proof_completed_resume(void) {
    PREPARE();s.PC=0x100d;budget=1;
    neverd_x64_continue_function_v6(&s,&budget,&f);
    budget=8;
    uint64_t status=neverd_x64_run_function_v6(&s,&budget,&env_budget,&f,declined,&attempts);
    return status!=3 || budget!=8 || env_budget!=4 || attempts!=0 || s.PC!=0x1010;
}
int proof_zero_budget(void) {
    PREPARE();budget=0;
    uint64_t status=neverd_x64_run_function_v6(&s,&budget,&env_budget,&f,declined,&attempts);
    return status!=9 || f.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        s.PC!=0x1000 || s.GPR[4]!=f.ReturnSlot || env_budget!=4 || attempts!=0;
}
int proof_uninitialized(void) {
    PREPARE();f.Phase=NEVERD_X64_FUNCTION_UNINITIALIZED;
    uint64_t status=neverd_x64_run_function_v6(&s,&budget,&env_budget,&f,declined,&attempts);
    return status!=5 || budget!=8 || env_budget!=4 || attempts!=0 || s.PC!=0x1000;
}
int proof_invalid(void) {
    PREPARE();s.CR8=16;
    uint64_t status=neverd_x64_run_function_v6(&s,&budget,&env_budget,&f,declined,&attempts);
    return status!=5 || budget!=8 || env_budget!=4 || attempts!=0 ||
        f.Phase!=NEVERD_X64_FUNCTION_ACTIVE || s.PC!=0x1000 || s.GPR[4]!=f.ReturnSlot;
}
int proof_invalid_return(void) {
    PREPARE();s.PC=0x100d;s.CR8=16;
    uint64_t status=neverd_x64_continue_function_v6(&s,&budget,&f);
    return status!=5 || budget!=8 || f.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        s.PC!=0x100d || s.GPR[4]!=f.ReturnSlot;
}
int proof_native(void) {
    PREPARE();s.PC=0x1030;neverd_x64_begin_function_v6(&s,&f);
    uint64_t status=neverd_x64_continue_function_v6(&s,&budget,&f);
    return status!=4 || budget!=8 || f.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        s.PC!=0x1030 || s.GPR[4]!=f.ReturnSlot;
}
int proof_environment_boundary(void) {
    PREPARE();s.PC=0x7777;s.GPR[4]+=8;
    uint64_t status=neverd_x64_run_function_v6(&s,&budget,&env_budget,&f,declined,&attempts);
    return status!=8 || budget!=8 || env_budget!=3 || attempts!=1 ||
        f.Phase!=NEVERD_X64_FUNCTION_ACTIVE || s.PC!=0x7777;
}
int proof_raw_adapter(void) {
    PREPARE();stack[11]=0x7777;
    uint64_t status=neverd_x64_continue_v6(&s,&budget);
    return status!=3 || budget!=0 || s.PC!=0x7777 || s.GPR[0]!=1 ||
        f.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
int proof_begin_rejects_descendant(void) {
    PREPARE();s.PC=0x1020;s.GPR[4]+=8;
    return neverd_x64_begin_function_v6(&s,&f)!=0 ||
        f.ReturnSlot!=(uintptr_t)&stack[8] || f.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
)C";
    for (bool Direct : {false, true}) {
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(Direct);
        SCOPED_TRACE(LLVM);
        auto [Source, Manifest] = source(
            request(
                std::string(
                    R"("entries":[4096,4112,4144],"callee_depth":1,"follow_internal_calls":true,"function_context":true,"dispatch":")") +
                (Direct ? "direct" : "switch") + "\""),
            LLVM);
        auto Parsed = llvm::json::parse(Manifest);
        ASSERT_TRUE(bool(Parsed));
        const auto *Contract =
            Parsed->getAsObject()->getObject("function_context");
        ASSERT_NE(Contract, nullptr);
        EXPECT_EQ(Contract->getString("context_type"),
                  "neverd_x64_function_context_v1");
        EXPECT_EQ(Contract->getString("environment_driver"),
                  "neverd_x64_run_function_v6");
        EXPECT_EQ(Contract->getBoolean("frame_ownership_proven"), false);

        if (Run) {
          execute(Source, std::string(Seed) + Probes + R"C(
int main(void) {
    for (uint64_t incoming=0;incoming<16;++incoming) {
        CHECK(proof_return(incoming<<60)==0);
        CHECK(proof_pause(incoming<<60)==0);
    }
    CHECK(proof_return(UINT64_MAX)==0);
    CHECK(proof_pause(UINT64_MAX)==0);
    CHECK(proof_changed_return()==0);
    CHECK(proof_pivot()==0);
    CHECK(proof_last_instruction()==0);
    CHECK(proof_completed_resume()==0);
    CHECK(proof_zero_budget()==0);
    CHECK(proof_uninitialized()==0);
    CHECK(proof_invalid()==0);
    CHECK(proof_invalid_return()==0);
    CHECK(proof_native()==0);
    CHECK(proof_environment_boundary()==0);
    CHECK(proof_raw_adapter()==0);
    CHECK(proof_begin_rejects_descendant()==0);
    return 0;
}
)C");
          continue;
        }
        const auto CFile = tmpFile("function-context.c");
        // No executable is built or run by the compile-only test. Keep the
        // instruction bodies, dispatchers and activation probes unchanged.
        std::ofstream(CFile) << Source << Seed << Probes;
        for (const char *Level : {"-O0", "-O2"}) {
          auto Built = exec(NEVERD_TEST_CLANG,
                            {"-std=c11", Level, "-Werror=uninitialized",
                             "-Werror=return-type", "-c", CFile.string(), "-o",
                             tmpFile("function-context.o").string()});
          ASSERT_TRUE(Built.ok()) << Built.err;
        }
      }
    }
  }
};

TEST_F(X64FunctionContextTest, CompileOnly) { checkFunctionContext(false); }
TEST_F(X64FunctionContextTest, RuntimeBoundaries) {
  checkFunctionContext(true);
}
