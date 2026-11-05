//===- X64FunctionCallTests.inc - Checked call view proofs ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Static request, compilation and wrapper proofs. No generated code runs.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest, FunctionCallsRejectUnboundDeclarations) {
  load({0xc3});
  const std::string Prefix =
      R"("entries":[4096],"follow_internal_calls":true,"function_context":true,"function_calls":)";
  for (
      const auto &Bad : {
          "null",
          "false",
          "{}",
          "[]",
          "[1]",
          R"([{"entry":4096,"abi":"sysv","argument_bytes":[],"result_bytes":8}])",
          R"([{"entry":4096,"argument_bytes":[],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[]}])",
          R"([{"entry":4112,"abi":"win64","argument_bytes":[],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[0],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[3],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[16],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[-1],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[1.5],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[null,0],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[null,true],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[null,"8"],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[null,{}],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[null,[]],"result_bytes":8}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":0}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":3}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":null}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":8,"name":"f"}])",
          R"([{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":8},{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":4}])",
      }) {
    SCOPED_TRACE(Bad);
    const auto Text = request(Prefix + Bad);
    const char *Report = "old";
    EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Text.data(),
                                                Text.size(), 0, &Report),
              nullptr);
    EXPECT_EQ(Report, nullptr);
    EXPECT_FALSE(take(neverd_last_error(Session)).empty());
  }
  const auto Unbound = request(
      R"("entries":[4096],"follow_internal_calls":true,"function_calls":[{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":8}])");
  const char *Report = "old";
  EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Unbound.data(),
                                              Unbound.size(), 0, &Report),
            nullptr);
  EXPECT_EQ(Report, nullptr);
  for (const char *Width : {"8", "null"}) {
    std::string TooMany =
        Prefix + R"([{"entry":4096,"abi":"win64","argument_bytes":[)";
    for (unsigned P = 0; P < 33; ++P) {
      if (P)
        TooMany += ',';
      TooMany += Width;
    }
    TooMany += R"(],"result_bytes":8}])";
    const auto Bounded = request(TooMany);
    EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Bounded.data(),
                                                Bounded.size(), 0, &Report),
              nullptr);
    EXPECT_EQ(Report, nullptr);
  }
}

TEST_F(X64ContinuationExportTest, FunctionCallsCompileAndProveBoundaries) {
  // Read original private stack bytes before returning. They are not among
  // the declared ordinary parameters and the checked view must not seed them.
  std::vector<uint8_t> Code(49, 0x90);
  const uint8_t Read[] = {0x8b, 0x44, 0x24, 0xf8, 0xc3};
  std::copy(std::begin(Read), std::end(Read), Code.begin());
  Code[16] = 0xc3;
  // The context-only fifth slot and RDX are read by the selected body. A hole
  // in the scalar interface must not be interpreted as an unused native input.
  const uint8_t HoleRead[] = {0x8b, 0x44, 0x24, 0x2c, 0x31, 0xd0, 0xc3};
  std::copy(std::begin(HoleRead), std::end(HoleRead), Code.begin() + 32);
  Code[48] = 0xc3;
  load(Code, {{0x1000, 5}, {0x1010, 1}, {0x1020, 7}, {0x1030, 1}});
  const std::string Selection =
      R"("entries":[4096,4112,4128,4144],"follow_internal_calls":true,"function_context":true)";
  const std::string Calls =
      R"(,"function_calls":[{"entry":4096,"abi":"win64","argument_bytes":[1,2,4,8,4],"result_bytes":4},{"entry":4112,"abi":"win64","argument_bytes":[],"result_bytes":8},{"entry":4128,"abi":"win64","argument_bytes":[1,null,4,8,null,2,null],"result_bytes":4},{"entry":4144,"abi":"win64","argument_bytes":[null,null,null,null,null,null,null],"result_bytes":8}])";
  const char Oracle[] = R"C(
typedef struct { uint64_t GPR[16], PC; } neverd_x64_state_v6;
)C";
  // The real driver and its original instruction bodies are compiled below.
  // Wrapper-only proofs substitute independent byte RAM and the documented
  // activation outcomes. They do not prove native instructions or providers.
  const char Driver[] = R"C(
typedef int (*neverd_x64_environment_v6)(neverd_x64_state_v6 *,uint64_t,void *);
static unsigned attempts,mode;
static unsigned copies;
static uintptr_t copied_address;
static size_t copied_bytes;
static uint64_t model_stack[24];
static void model_copy(void *dest,uintptr_t address,size_t bytes) {
    if(address>sizeof(model_stack) || bytes>sizeof(model_stack)-address)
        __builtin_trap();
    ++copies;copied_address=address;copied_bytes=bytes;
    __builtin_memcpy(dest,(const uint8_t *)model_stack+address,bytes);
}
#define __builtin_memcpy(D,S,N) model_copy(D,(uintptr_t)(S),N)
__attribute__((always_inline))
uint64_t neverd_x64_run_function_v6(neverd_x64_state_v6 *s,uint64_t *b,
    uint64_t *e,neverd_x64_function_context_v1 *f,
    neverd_x64_environment_v6 provider,void *opaque) {
    if(f->Phase!=NEVERD_X64_FUNCTION_ACTIVE)
        return f->Phase==NEVERD_X64_FUNCTION_RETURNED?3:5;
    if(!*b)return 9;
    ++attempts;
    if(mode==1)return 3; /* An external return is not entry completion. */
    if(mode==2)return 4;
    if(mode==3){--*b;s->GPR[1]^=1;return 9;}
    if(s->PC==0x1020)
        s->GPR[0]=(uint32_t)(model_stack[13]>>32)^(uint32_t)s->GPR[2];
    --*b;f->Phase=NEVERD_X64_FUNCTION_RETURNED;return 3;
}
)C";
  const char Probes[] = R"C(
#define PREPARE() \
    uint64_t *stack=model_stack,budget=2,env_budget=3; \
    for(unsigned i=0;i<24;++i)stack[i]=0; \
    neverd_x64_state_v6 state={0};neverd_x64_call_context_v1 call={0}; \
    state.PC=0x1000;state.GPR[4]=64; \
    state.GPR[1]=0x1122334455667788ULL;state.GPR[2]=0x9988776655443322ULL; \
    state.GPR[8]=0xabcdef0187654321ULL;state.GPR[9]=UINT64_MAX; \
    stack[13]=0xfedcba9876543210ULL;attempts=0;mode=0; \
    copies=0;copied_address=0;copied_bytes=0
#define CALL() neverd_x64_call_1000_v6(&state,&budget,&env_budget,&call,0,0, \
    0x88,0x3322,0x87654321,UINT64_MAX,0x76543210)
#define RESUME() neverd_x64_resume_1000_v6(&state,&budget,&env_budget,&call,0,0)
int proof_call_value(uint64_t incoming) {
    PREPARE();state.GPR[0]=incoming;stack[7]=incoming;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=3 || r.Completed!=1 || r.Value!=(uint32_t)incoming ||
        call.Value!=r.Value || call.Entry!=0x1000 ||
        call.Function.ReturnSlot!=64 ||
        budget!=1 || env_budget!=3 || attempts!=1 ||
        stack[7]!=incoming || stack[13]!=0xfedcba9876543210ULL ||
        state.GPR[1]!=0x1122334455667788ULL ||
        state.GPR[2]!=0x9988776655443322ULL ||
        state.GPR[8]!=0xabcdef0187654321ULL || state.GPR[9]!=UINT64_MAX;
}
int proof_mismatched_register(void) {
    PREPARE();state.GPR[1]^=1;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || r.Value || call.Function.Phase ||
        call.Entry || call.Value || budget!=2 || env_budget!=3 || attempts;
}
int proof_mismatched_stack(void) {
    PREPARE();stack[13]^=1;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || r.Value || call.Function.Phase ||
        call.Entry || call.Value || budget!=2 || env_budget!=3 || attempts ||
        stack[13]!=0xfedcba9876543211ULL;
}
int proof_wrong_entry(void) {
    PREPARE();state.PC=0x1010;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || call.Function.Phase || attempts ||
        state.PC!=0x1010 || budget!=2 || env_budget!=3;
}
int proof_stack_wrap(void) {
    PREPARE();state.GPR[4]=UINT64_MAX-42;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || call.Function.Phase || attempts ||
        state.GPR[4]!=UINT64_MAX-42 || budget!=2 || env_budget!=3;
}
int proof_zero_budget(void) {
    PREPARE();budget=0;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=9 || r.Completed || r.Value || attempts || budget ||
        call.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE || env_budget!=3;
}
int proof_pause_does_not_rebind(uint64_t incoming) {
    PREPARE();mode=3;state.GPR[0]=incoming;
    neverd_x64_call_result_v1 r=CALL();
    int bad=r.Status!=9 || r.Completed || r.Value || budget!=1 || attempts!=1;
    mode=0;r=RESUME();
    return bad || r.Status!=3 || !r.Completed || r.Value!=(uint32_t)incoming ||
        budget || attempts!=2 || state.GPR[1]!=0x1122334455667789ULL;
}
int proof_call_cannot_restart_active(void) {
    PREPARE();mode=2;neverd_x64_call_result_v1 r=CALL();
    int bad=r.Status!=4 || r.Completed || r.Value || attempts!=1;
    mode=0;r=CALL();
    return bad || r.Status!=5 || r.Completed || r.Value || attempts!=1 ||
        call.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE || budget!=2;
}
int proof_return_status_is_not_completion(void) {
    PREPARE();mode=1;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=3 || r.Completed || r.Value || attempts!=1 ||
        call.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
int proof_completed_result_is_retained(uint64_t incoming) {
    PREPARE();state.GPR[0]=incoming;neverd_x64_call_result_v1 r=CALL();
    int bad=r.Value!=(uint32_t)incoming || !r.Completed;
    state.GPR[0]=~incoming;state.PC=0x7777;budget=17;env_budget=19;r=RESUME();
    return bad || r.Status!=3 || !r.Completed || r.Value!=(uint32_t)incoming ||
        attempts!=1 || budget!=17 || env_budget!=19 || state.PC!=0x7777;
}
int proof_uninitialized_resume(void) {
    PREPARE();neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=5 || r.Completed || r.Value || attempts || budget!=2;
}
int proof_wrong_resume_interface(void) {
    PREPARE();mode=2;CALL();
    neverd_x64_call_result_v1 r=neverd_x64_resume_1010_v6(
        &state,&budget,&env_budget,&call,0,0);
    return r.Status!=5 || r.Completed || r.Value || attempts!=1 || budget!=2;
}
int proof_no_arguments_full_result(uint64_t incoming) {
    PREPARE();state.PC=0x1010;state.GPR[0]=incoming;
    neverd_x64_call_result_v1 r=neverd_x64_call_1010_v6(
        &state,&budget,&env_budget,&call,0,0);
    return r.Status!=3 || !r.Completed || r.Value!=incoming || attempts!=1;
}
#define HOLE_PREPARE() \
    PREPARE();state.PC=0x1020;stack[14]=0xfedcba987654beefULL
#define HOLE_CALL() neverd_x64_call_1020_v6( \
    &state,&budget,&env_budget,&call,0,0,0x88,0x87654321,UINT64_MAX,0xbeef)
int proof_native_slots_are_retained(uint64_t incoming,uint64_t native_rdx) {
    HOLE_PREPARE();stack[13]=incoming;state.GPR[2]=native_rdx;
    neverd_x64_call_result_v1 r=HOLE_CALL();
    return r.Status!=3 || !r.Completed ||
        r.Value!=((uint32_t)(incoming>>32)^(uint32_t)native_rdx) ||
        stack[13]!=incoming || state.GPR[2]!=native_rdx ||
        stack[14]!=0xfedcba987654beefULL || budget!=1 || attempts!=1 ||
        copies!=1 || copied_address!=112 || copied_bytes!=2;
}
int proof_later_scalar_still_checked(uint64_t incoming) {
    HOLE_PREPARE();stack[13]=incoming;stack[14]^=1;
    neverd_x64_call_result_v1 r=HOLE_CALL();
    return r.Status!=5 || r.Completed || r.Value || attempts || budget!=2 ||
        call.Function.Phase || call.Entry || stack[13]!=incoming ||
        copies!=1 || copied_address!=112 || copied_bytes!=2;
}
int proof_native_slots_survive_pause(uint64_t first,uint64_t second) {
    HOLE_PREPARE();stack[13]=first;state.GPR[2]=first;mode=3;
    neverd_x64_call_result_v1 r=HOLE_CALL();
    int bad=r.Status!=9 || r.Completed || attempts!=1 || budget!=1 ||
        stack[13]!=first || state.GPR[2]!=first;
    stack[13]=second;state.GPR[2]=~second;stack[14]^=1;mode=0;
    r=neverd_x64_resume_1020_v6(&state,&budget,&env_budget,&call,0,0);
    return bad || r.Status!=3 || !r.Completed ||
        r.Value!=((uint32_t)(second>>32)^(uint32_t)~second) || budget ||
        attempts!=2 || copies!=1 || stack[13]!=second || state.GPR[2]!=~second;
}
int proof_context_only_interface_does_not_read_slots(void) {
    PREPARE();state.PC=0x1030;state.GPR[4]=UINT64_MAX-8;mode=2;
    neverd_x64_call_result_v1 r=neverd_x64_call_1030_v6(
        &state,&budget,&env_budget,&call,0,0);
    return r.Status!=4 || r.Completed || r.Value || copies || attempts!=1 ||
        call.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        call.Function.ReturnSlot!=UINT64_MAX-8 || budget!=2 || env_budget!=3;
}
)C";
  for (bool Direct : {false, true})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Direct);
      SCOPED_TRACE(LLVM);
      const std::string Dispatch =
          Direct ? R"(,"dispatch":"direct")" : R"(,"dispatch":"switch")";
      auto [Source, Manifest] =
          source(request(Selection + Dispatch + Calls), LLVM);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed));
      const auto *Interfaces =
          Parsed->getAsObject()->getObject("function_calls");
      ASSERT_NE(Interfaces, nullptr);
      const auto *Entries = Interfaces->getArray("interfaces");
      ASSERT_NE(Entries, nullptr);
      ASSERT_EQ(Entries->size(), 4u);
      const auto *First = Entries->front().getAsObject();
      ASSERT_NE(First, nullptr);
      EXPECT_EQ(First->getBoolean("source_abi_inferred"), false);
      const auto *Arguments = First->getArray("arguments");
      ASSERT_NE(Arguments, nullptr);
      ASSERT_EQ(Arguments->size(), 5u);
      for (unsigned P = 0; P < 4; ++P)
        EXPECT_EQ((*Arguments)[P].getAsObject()->getInteger("gpr_index"),
                  (std::vector<int>{1, 2, 8, 9})[P]);
      EXPECT_EQ((*Arguments)[4].getAsObject()->getInteger("entry_sp_offset"),
                40);
      const auto *Holes = (*Entries)[2].getAsObject()->getArray("arguments");
      ASSERT_NE(Holes, nullptr);
      ASSERT_EQ(Holes->size(), 7u);
      for (size_t P : {1, 4, 6}) {
        const auto *Slot = (*Holes)[P].getAsObject();
        EXPECT_EQ(Slot->getBoolean("retained_in_state"), true);
        EXPECT_FALSE(Slot->get("bytes"));
      }
      EXPECT_EQ((*Holes)[1].getAsObject()->getInteger("gpr_index"), 2);
      EXPECT_EQ((*Holes)[4].getAsObject()->getInteger("entry_sp_offset"), 40);
      EXPECT_EQ((*Holes)[5].getAsObject()->getInteger("entry_sp_offset"), 48);
      EXPECT_EQ((*Holes)[5].getAsObject()->getInteger("bytes"), 2);
      EXPECT_EQ((*Holes)[6].getAsObject()->getInteger("entry_sp_offset"), 56);
      const auto SparseStart =
          Source.find("neverd_x64_call_result_v1 neverd_x64_call_1020_v6(");
      ASSERT_NE(SparseStart, std::string::npos);
      const auto SparseCall = Source.substr(SparseStart);
      EXPECT_NE(SparseCall.find("UINT64_MAX - 49ULL"), std::string::npos);
      EXPECT_EQ(SparseCall.find("stack_arg4"), std::string::npos);
      EXPECT_EQ(SparseCall.find("stack_arg6"), std::string::npos);

      const auto Marker = Source.find("/* Checked scalar call interfaces.");
      ASSERT_NE(Marker, std::string::npos);
      const auto Base = source(request(Selection + Dispatch), LLVM);
      // All original instruction bodies and drivers retain exactly their text.
      EXPECT_EQ(Source.substr(0, Marker), Base.first + "\n");
      const auto CFile = tmpFile("checked-function-calls.c");
      std::ofstream(CFile) << Source;
      for (const char *Level : {"-O0", "-O2"}) {
        auto Built = exec(NEVERD_TEST_CLANG,
                          {"-std=c11", Level, "-Werror=uninitialized",
                           "-Werror=return-type", "-c", CFile.string(), "-o",
                           tmpFile("checked-function-calls.o").string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
      }
      const auto Begin = Source.find("enum neverd_x64_function_phase_v1");
      const auto Function =
          Source.find("int neverd_x64_begin_function_v6", Begin);
      const auto End = Source.find("\n}\n", Function);
      ASSERT_NE(Begin, std::string::npos);
      ASSERT_NE(End, std::string::npos);
      const auto Proof = tmpFile("checked-call-proofs.c");
      std::string Wrappers = Source.substr(Marker);
      const std::string Definition = "\nneverd_x64_call_result_v1 neverd_x64_";
      const std::string Inline = "__attribute__((always_inline)) ";
      for (size_t At = 0;
           (At = Wrappers.find(Definition, At)) != std::string::npos;
           At += Definition.size() + Inline.size())
        Wrappers.insert(At + 1, Inline);
      // Force only inlining; the generated wrapper statements are unchanged.
      std::ofstream(Proof) << "#include <stdint.h>\n#include <stddef.h>\n"
                           << Oracle << Source.substr(Begin, End + 3 - Begin)
                           << Driver << Wrappers << Probes;
      const auto IR = tmpFile("checked-call-proofs.ll");
      auto Built =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", "-O2", "-mllvm", "-inline-threshold=10000", "-S",
                "-emit-llvm", Proof.string(), "-o", IR.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      llvm::LLVMContext Context;
      llvm::SMDiagnostic Diagnostic;
      auto Module = llvm::parseIRFile(IR.string(), Diagnostic, Context);
      ASSERT_NE(Module, nullptr);
      unsigned Count = 0;
      for (const auto &F : *Module)
        if (F.getName().starts_with("proof_")) {
          SCOPED_TRACE(F.getName().str());
          ++Count;
          ASSERT_EQ(F.size(), 1u);
          const auto *Return =
              llvm::dyn_cast<llvm::ReturnInst>(F.front().getTerminator());
          ASSERT_NE(Return, nullptr);
          const auto *Value =
              llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
          ASSERT_NE(Value, nullptr);
          EXPECT_TRUE(Value->isZero());
        }
      EXPECT_EQ(Count, 17u);
    }
}

TEST_F(X64ContinuationExportTest,
       FunctionCallsAdmitScalarWidthAndArgumentLimits) {
  load({0xc3});
  for (unsigned Bytes : {1, 2, 4, 8})
    for (unsigned Count : {0, 4, 5, 32}) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(Count);
      std::string Selection =
          R"("entries":[4096],"follow_internal_calls":true,"function_context":true,"function_calls":[{"entry":"0x1000","abi":"win64","argument_bytes":[)";
      for (unsigned P = 0; P < Count; ++P) {
        if (P)
          Selection += ',';
        Selection += std::to_string(Bytes);
      }
      Selection += "],\"result_bytes\":" + std::to_string(Bytes) + "}]";
      for (bool LLVM : {false, true}) {
        auto [Source, Manifest] = source(request(Selection), LLVM);
        auto Parsed = llvm::json::parse(Manifest);
        ASSERT_TRUE(bool(Parsed));
        const auto *Entries = Parsed->getAsObject()
                                  ->getObject("function_calls")
                                  ->getArray("interfaces");
        ASSERT_NE(Entries, nullptr);
        ASSERT_EQ(Entries->size(), 1u);
        const auto *Interface = Entries->front().getAsObject();
        EXPECT_EQ(Interface->getInteger("result_bytes"), Bytes);
        const auto *Arguments = Interface->getArray("arguments");
        ASSERT_NE(Arguments, nullptr);
        ASSERT_EQ(Arguments->size(), Count);
        if (Count == 32)
          EXPECT_EQ(
              Arguments->back().getAsObject()->getInteger("entry_sp_offset"),
              256);
        const auto File = tmpFile("scalar-call-width.c");
        std::ofstream(File) << Source;
        const auto Built = exec(NEVERD_TEST_CLANG,
                                {"-std=c11", "-O0", "-c", File.string(), "-o",
                                 tmpFile("scalar-call-width.o").string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
      }
    }
}

TEST_F(X64ContinuationExportTest, FunctionCallsAdmitContextOnlyArgumentLimit) {
  load({0xc3});
  std::string Selection =
      R"("entries":[4096],"follow_internal_calls":true,"function_context":true,"function_calls":[{"entry":4096,"abi":"win64","argument_bytes":[)";
  for (unsigned P = 0; P < 32; ++P)
    Selection += P ? ",null" : "null";
  Selection += R"(],"result_bytes":8}])";
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(request(Selection), LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    const auto *Entries = Parsed->getAsObject()
                              ->getObject("function_calls")
                              ->getArray("interfaces");
    ASSERT_NE(Entries, nullptr);
    ASSERT_EQ(Entries->size(), 1u);
    const auto *Arguments =
        Entries->front().getAsObject()->getArray("arguments");
    ASSERT_NE(Arguments, nullptr);
    ASSERT_EQ(Arguments->size(), 32u);
    for (const auto &Argument : *Arguments) {
      EXPECT_EQ(Argument.getAsObject()->getBoolean("retained_in_state"), true);
      EXPECT_FALSE(Argument.getAsObject()->get("bytes"));
    }
    EXPECT_EQ(Arguments->back().getAsObject()->getInteger("entry_sp_offset"),
              256);
    const auto File = tmpFile("context-only-call.c");
    std::ofstream(File) << Source;
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-std=c11", "-O0", "-c", File.string(), "-o",
                                 tmpFile("context-only-call.o").string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
  }
}
