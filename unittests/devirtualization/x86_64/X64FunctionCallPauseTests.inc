//===- X64FunctionCallPauseTests.inc - Pre-CALL handoff proofs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile original instruction handoffs without executing generated code.
//===----------------------------------------------------------------------===//

class X64CallPauseTest : public X64ContinuationExportTest {
protected:
  void prove(const std::string &Source, llvm::StringRef Probes,
             unsigned Expected) {
    const auto File = tmpFile("call-pause.c");
    std::ofstream(File) << Source;
    // Compile the exact production source, including direct-region noinline.
    for (const char *Level : {"-O0", "-O2"}) {
      const auto Built =
          exec(NEVERD_TEST_CLANG, {"-std=c11", Level, "-Werror=uninitialized",
                                   "-Werror=return-type", "-c", File.string(),
                                   "-o", tmpFile("call-pause.o").string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
    }
    // These tiny compile-time predicates need interprocedural value folding.
    // Remove only the direct dispatcher's performance annotation in this proof
    // copy; all original instruction bodies, drivers and bridges stay intact.
    // The exact, annotated source was compiled separately above.
    std::string Proof = Source;
    const std::string Annotation = "__attribute__((noinline))";
    for (auto At = Proof.find(Annotation); At != std::string::npos;
         At = Proof.find(Annotation, At))
      Proof.erase(At, Annotation.size());
    std::ofstream(File) << Proof << Probes.str();
    const auto IR = tmpFile("call-pause.ll");
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", "-O2", "-mllvm", "-inline-threshold=10000", "-mllvm",
              "-inline-cold-callsite-threshold=10000", "-S", "-emit-llvm",
              File.string(), "-o", IR.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    const auto Module = llvm::parseIRFile(IR.string(), Diagnostic, Context);
    ASSERT_NE(Module, nullptr);
    // The fixture owns accessible, contiguous RAM at [0x10000, 0x10080),
    // disjoint from host state and provider storage. LLVM does not infer
    // disjointness or byte overlap between unrelated inttoptr constants.
    // Express that input memory contract as one object before value folding.
    // Only constant-address loads/stores in that region are relocated; the
    // instruction bodies, integer addresses and computed state stay intact.
    constexpr uint64_t GuestBase = 0x10000, GuestBytes = 128;
    auto *GuestType =
        llvm::ArrayType::get(llvm::Type::getInt8Ty(Context), GuestBytes);
    auto *Guest = new llvm::GlobalVariable(*Module, GuestType, false,
                                           llvm::GlobalValue::ExternalLinkage,
                                           nullptr, "neverd_test_guest_stack");
    Guest->setAlignment(llvm::Align(64));
    for (auto &Function : *Module)
      if (Function.getName().starts_with("proof_")) {
        unsigned Relocated = 0;
        for (auto &Block : Function)
          for (auto &Instruction : Block) {
            const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
            const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
            if (!Load && !Store)
              continue;
            const unsigned PointerIndex = Load ? 0 : 1;
            const auto *Pointer = llvm::dyn_cast<llvm::ConstantExpr>(
                Instruction.getOperand(PointerIndex));
            if (!Pointer || Pointer->getOpcode() != llvm::Instruction::IntToPtr)
              continue;
            const auto *Address =
                llvm::dyn_cast<llvm::ConstantInt>(Pointer->getOperand(0));
            ASSERT_NE(Address, nullptr);
            const auto At = Address->getZExtValue();
            const auto Width = Module->getDataLayout().getTypeStoreSize(
                Load ? Load->getType() : Store->getValueOperand()->getType());
            ASSERT_GE(At, GuestBase);
            ASSERT_LE(At, GuestBase + GuestBytes);
            ASSERT_LE(Width.getFixedValue(), GuestBase + GuestBytes - At);
            llvm::IRBuilder<> Builder(&Instruction);
            Instruction.setOperand(
                PointerIndex,
                Builder.CreateInBoundsGEP(
                    GuestType, Guest,
                    {Builder.getInt64(0), Builder.getInt64(At - GuestBase)}));
            ++Relocated;
          }
        EXPECT_GT(Relocated, 0u);
      }
    const auto MappedIR = tmpFile("call-pause-mapped.ll");
    std::string Mapped;
    llvm::raw_string_ostream Stream(Mapped);
    Module->print(Stream, nullptr);
    std::ofstream(MappedIR) << Mapped;
    const auto OptimizedIR = tmpFile("call-pause-proved.ll");
    if (!neverd::test::compilerReadsCurrentIR(NEVERD_TEST_CLANG))
      GTEST_SKIP() << "this Clang cannot read the linked LLVM's IR";
    const auto Optimized =
        exec(NEVERD_TEST_CLANG, {"-O2", "-S", "-emit-llvm", MappedIR.string(),
                                 "-o", OptimizedIR.string()});
    ASSERT_TRUE(Optimized.ok()) << Optimized.err;
    const auto Proved =
        llvm::parseIRFile(OptimizedIR.string(), Diagnostic, Context);
    ASSERT_NE(Proved, nullptr);
    unsigned Count = 0;
    for (const auto &Function : *Proved)
      if (Function.getName().starts_with("proof_")) {
        SCOPED_TRACE(Function.getName().str());
        ++Count;
        ASSERT_EQ(Function.size(), 1u);
        const auto *Return =
            llvm::dyn_cast<llvm::ReturnInst>(Function.front().getTerminator());
        ASSERT_NE(Return, nullptr);
        const auto *Value =
            llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
        ASSERT_NE(Value, nullptr);
        EXPECT_TRUE(Value->isZero());
      }
    EXPECT_EQ(Count, Expected);
  }

  static constexpr const char *Seed = R"C(
static int environment(neverd_x64_state_v6 *s,uint64_t status,void *ctx) {
    ++*(unsigned *)ctx;return 0;
}
static neverd_x64_state_v6 seed(void) {
    neverd_x64_state_v6 s={0};s.RFlags=2;s.PC=0x1000;
    s.CS=(neverd_x64_segment){0,0xffffffff,8,0xa09b};
    s.GPR[4]=0x10040;return s;
}
#define PREPARE() \
    uint64_t *stack=(uint64_t *)(uintptr_t)0x10000; \
    neverd_x64_state_v6 state=seed(); \
    neverd_x64_function_context_v1 parent={0}; \
    unsigned environment_calls=0; \
    uint64_t budget=10,environment_budget=7; \
    stack[7]=0xdeadbeef;stack[8]=0x9000; \
    neverd_x64_begin_function_v6(&state,&parent)
#define RUN() neverd_x64_run_function_v6(&state,&budget,&environment_budget, \
    &parent,environment,&environment_calls)
)C";
};

TEST_F(X64CallPauseTest, RequiresBoundSitesAndPreservesDisabledSource) {
  load({0xe8, 0x01, 0, 0, 0, 0xc3, 0xc3}, {{0x1000, 6}, {0x1006, 1}});
  const std::string Base =
      R"("entries":[4096,4102],"follow_internal_calls":true,"function_context":true,"function_calls":[{"entry":4102,"abi":"win64","argument_bytes":[],"result_bytes":8,"call_sites":[4096]}])";
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      const auto Selection = Base + ",\"dispatch\":\"" + Dispatch + "\"";
      const auto Original = source(request(Selection), LLVM);
      const auto Disabled =
          source(request(Selection + ",\"pause_call_sites\":false"), LLVM);
      EXPECT_TRUE(Original.first == Disabled.first);
      auto OriginalManifest = llvm::json::parse(Original.second);
      auto DisabledManifest = llvm::json::parse(Disabled.second);
      ASSERT_TRUE(bool(OriginalManifest))
          << llvm::toString(OriginalManifest.takeError());
      ASSERT_TRUE(bool(DisabledManifest))
          << llvm::toString(DisabledManifest.takeError());
      auto *Echo = DisabledManifest->getAsObject()->getObject("request");
      ASSERT_NE(Echo, nullptr);
      EXPECT_EQ(Echo->getBoolean("pause_call_sites"), false);
      Echo->erase("pause_call_sites");
      EXPECT_TRUE(*OriginalManifest == *DisabledManifest);
      const auto [Text, Report] =
          source(request(Selection + ",\"pause_call_sites\":true"), LLVM);
      EXPECT_NE(Text.find("return NEVERD_X64_CALL_SITE_READY_V1;"),
                std::string::npos);
      auto Manifest = llvm::json::parse(Report);
      ASSERT_TRUE(bool(Manifest)) << llvm::toString(Manifest.takeError());
      const auto *Pause = Manifest->getAsObject()->getObject("call_site_pause");
      ASSERT_NE(Pause, nullptr);
      EXPECT_EQ(Pause->getInteger("status"), 21);
      for (const char *Bad : {"null", "1", "\"true\"", "[]", "{}"}) {
        const auto Request =
            request(Selection + ",\"pause_call_sites\":" + Bad);
        const char *Report = "old";
        EXPECT_EQ(neverd_x64_continuation_source_v1(
                      Session, Request.data(), Request.size(), LLVM, &Report),
                  nullptr);
        EXPECT_EQ(Report, nullptr);
      }
    }
  for (
      const char *Selection :
      {R"("entries":[4096],"pause_call_sites":true)",
       R"("entries":[4096],"follow_internal_calls":true,"function_context":true,"pause_call_sites":true)",
       R"("entries":[4096],"follow_internal_calls":true,"function_context":true,"pause_call_sites":true,"function_calls":[{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":8}])"}) {
    const auto Request = request(Selection);
    const char *Report = "old";
    EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Request.data(),
                                                Request.size(), 0, &Report),
              nullptr);
    EXPECT_EQ(Report, nullptr);
    EXPECT_NE(
        take(neverd_last_error(Session)).find("pause_call_sites requires"),
        std::string::npos);
  }
}

TEST_F(X64CallPauseTest, PreservesOriginalPrefixStackAndCompletion) {
  std::vector<uint8_t> Code(37, 0x90);
  const uint8_t Prefix[] = {0xb9, 0x44, 0, 0, 0, 0xe8, 0x16, 0, 0, 0, 0xc3};
  const uint8_t Body[] = {0x8b, 0x44, 0x24, 0x2c, 0xc3};
  std::copy(std::begin(Prefix), std::end(Prefix), Code.begin());
  std::copy(std::begin(Body), std::end(Body), Code.begin() + 32);
  load(Code, {{0x1000, 11}, {0x1020, 5}});
  const std::string Base =
      R"("entries":[4096,4128],"follow_internal_calls":true,"function_context":true,"pause_call_sites":true,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[4,null,null,null,null],"result_bytes":4,"call_sites":[4101]}])";
  const std::string Probes = std::string(Seed) + R"C(
int proof_ready_before_call(uint64_t word) {
    PREPARE();stack[12]=word;
    uint64_t status=RUN();
    return status!=21 || state.PC!=0x1005 || state.GPR[4]!=0x10040 ||
        state.GPR[1]!=0x44 || budget!=9 || environment_budget!=7 ||
        environment_calls || stack[7]!=0xdeadbeef || stack[12]!=word ||
        parent.Phase!=NEVERD_X64_FUNCTION_ACTIVE || parent.ReturnSlot!=0x10040;
}
int proof_ready_is_repeatable(void) {
    PREPARE();RUN();uint64_t status=RUN();
    return status!=21 || state.PC!=0x1005 || budget!=9 ||
        stack[7]!=0xdeadbeef || environment_calls || environment_budget!=7;
}
int proof_budget_precedes_ready(void) {
    PREPARE();budget=1;uint64_t first=RUN();
    if(first!=9 || state.PC!=0x1005 || budget)return 1;
    budget=1;uint64_t next=RUN();
    return next!=21 || budget!=1 || stack[7]!=0xdeadbeef || environment_calls;
}
int proof_inactive_context_never_hands_off(void) {
    PREPARE();state.PC=0x1005;parent.Phase=0;
    return RUN()!=5 || budget!=10 || stack[7]!=0xdeadbeef || environment_calls;
}
int proof_bridge_keeps_instruction_validation(void) {
    PREPARE();state.PC=0x1005;state.GPR[1]=0x44;state.RFlags=0;
    if(RUN()!=21)return 1;
    neverd_x64_call_site_context_v1 child={0};
    neverd_x64_call_result_v1 r=neverd_x64_call_site_1005_v6(
        &state,&budget,&environment_budget,&child,environment,&environment_calls,0x44);
    return r.Status!=5 || r.Completed || child.Callee.Function.Phase ||
        child.Prepared!=1 || budget!=10 || state.PC!=0x1005 ||
        state.GPR[4]!=0x10040 || stack[7]!=0xdeadbeef || environment_calls;
}
int proof_original_bridge_and_parent(uint64_t word) {
    PREPARE();stack[12]=word;
    if(RUN()!=21)return 1;
    neverd_x64_call_site_context_v1 child={0};
    neverd_x64_call_result_v1 r=neverd_x64_call_site_1005_v6(
        &state,&budget,&environment_budget,&child,environment,&environment_calls,0x44);
    if(r.Status!=3 || r.Completed!=1 || r.Value!=(uint32_t)(word>>32) ||
       child.Callee.Function.ReturnSlot!=0x10038 || state.PC!=0x100a ||
       state.GPR[4]!=0x10040 || budget!=6 || stack[7]!=0x100a ||
       stack[12]!=word || parent.Phase!=NEVERD_X64_FUNCTION_ACTIVE)return 2;
    uint64_t status=RUN();
    return status!=3 || parent.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        state.PC!=0x9000 || state.GPR[4]!=0x10048 || budget!=5 ||
        environment_budget!=7 || environment_calls || stack[12]!=word;
}
)C";
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(Dispatch);
      const auto [Text, Report] =
          source(request(Base + ",\"dispatch\":\"" + Dispatch + "\""), LLVM);
      prove(Text, Probes, 6);
    }
}

TEST_F(X64CallPauseTest, NestedBridgesKeepTheirOwnReturnCells) {
  std::vector<uint8_t> Code(68, 0x90);
  const uint8_t Prefix[] = {0xb9, 0x44, 0, 0, 0, 0xe8, 0x16, 0, 0, 0, 0xc3};
  const uint8_t Body[] = {0xe8, 0x1b, 0, 0, 0, 0xc3};
  const uint8_t Leaf[] = {0x44, 0x89, 0xd0, 0xc3};
  std::copy(std::begin(Prefix), std::end(Prefix), Code.begin());
  std::copy(std::begin(Body), std::end(Body), Code.begin() + 32);
  std::copy(std::begin(Leaf), std::end(Leaf), Code.begin() + 64);
  load(Code, {{0x1000, 11}, {0x1020, 6}, {0x1040, 4}});
  const std::string Base =
      R"("entries":[4096,4128,4160],"follow_internal_calls":true,"function_context":true,"pause_call_sites":true,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[],"result_bytes":8,"call_sites":[4101]},{"entry":4160,"abi":"win64","argument_bytes":[],"result_bytes":8,"call_sites":[4128]}])";
  const std::string Probes = std::string(Seed) + R"C(
int proof_nested_activations(uint64_t word) {
    PREPARE();state.GPR[10]=word;
    if(RUN()!=21)return 1;
    neverd_x64_call_site_context_v1 outer={0},inner={0};
    neverd_x64_call_result_v1 r=neverd_x64_call_site_1005_v6(
        &state,&budget,&environment_budget,&outer,environment,&environment_calls);
    if(r.Status!=21 || r.Completed || state.PC!=0x1020 || budget!=8 ||
       outer.Callee.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
       outer.Callee.Function.ReturnSlot!=0x10038 || stack[7]!=0x100a)return 2;
    r=neverd_x64_call_site_1020_v6(
        &state,&budget,&environment_budget,&inner,environment,&environment_calls);
    if(r.Status!=3 || !r.Completed || r.Value!=(uint32_t)word ||
       inner.Callee.Function.ReturnSlot!=0x10030 || state.PC!=0x1025 ||
       state.GPR[4]!=0x10038 || budget!=5 || stack[6]!=0x1025)return 3;
    r=neverd_x64_resume_site_1005_v6(
        &state,&budget,&environment_budget,&outer,environment,&environment_calls);
    if(r.Status!=3 || !r.Completed || r.Value!=(uint32_t)word ||
       state.PC!=0x100a || state.GPR[4]!=0x10040 || budget!=4 ||
       stack[7]!=0x100a || parent.Phase!=NEVERD_X64_FUNCTION_ACTIVE)return 4;
    return RUN()!=3 || state.PC!=0x9000 || state.GPR[4]!=0x10048 || budget!=3 ||
        parent.Phase!=NEVERD_X64_FUNCTION_RETURNED || environment_calls ||
        environment_budget!=7;
}
)C";
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(Dispatch);
      const auto [Text, Report] =
          source(request(Base + ",\"dispatch\":\"" + Dispatch + "\""), LLVM);
      prove(Text, Probes, 1);
    }
}
