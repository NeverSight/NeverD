//===- X64ScheduledCallTests.inc - Scheduled native call views ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile event-aware CALL bridges and prove their original state effects.
/// Generated programs, instruction executors and guests are never executed.
//===----------------------------------------------------------------------===//

#include "X64ScheduledCallProof.inc"

class X64ScheduledCallTest : public X64PagedFunctionCallSiteTest {
protected:
  static std::string selection(bool Paged, bool Sites, bool Pause,
                               bool Scheduled = true) {
    return request(
        (R"("entries":[4096,4112,4128],"follow_internal_calls":true,"function_context":true,"paged_memory":)" +
         llvm::Twine(Paged ? "true" : "false") +
         (Paged ? "" : R"(,"event_delivery":true)") +
         R"(,"scheduled_events":)" + (Scheduled ? "true" : "false") +
         R"(,"pause_call_sites":)" + (Pause ? "true" : "false") +
         R"(,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[1,null,4,8,null,null],"result_bytes":4)" +
         (Sites ? R"(,"call_sites":[4101,4112])" : "") + "}]")
            .str());
  }

  std::pair<std::string, std::string> exportCalls(bool Paged, bool LLVM,
                                                  bool Sites, bool Pause) {
    const auto Request = selection(Paged, Sites, Pause);
    const char *Report = nullptr;
    auto Source = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), LLVM, &Report));
    EXPECT_FALSE(Source.empty()) << take(neverd_last_error(Session));
    return {std::move(Source), take(Report)};
  }

  void proveCalls(bool Paged, bool LLVM, llvm::StringRef Source) {
    const std::string Name = std::string("scheduled-calls-") +
                             (Paged ? "paged-" : "plain-") +
                             (LLVM ? "llvm" : "high");
    const auto File = tmpFile(Name + ".c");
    const auto InitialIR = tmpFile(Name + ".initial.ll");
    const auto IR = tmpFile(Name + ".ll");
    std::ofstream(File) << Source.str() << "\n#define PAGED " << Paged << "\n"
                        << ScheduledCallProbes;
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", "-O2", "-funroll-loops", "-mllvm",
                       "-unroll-count=16", "-mllvm", "-unroll-threshold=100000",
                       "-mllvm", "-inline-threshold=10000", "-mllvm",
                       "-inline-cold-callsite-threshold=10000", "-S",
                       "-emit-llvm", File.string(), "-o", InitialIR.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto InputIR = InitialIR;
    if (!Paged) {
      llvm::LLVMContext Context;
      llvm::SMDiagnostic Diagnostic;
      const auto Module =
          llvm::parseIRFile(InitialIR.string(), Diagnostic, Context);
      ASSERT_NE(Module, nullptr);
      // The fixture declares accessible, disjoint RAM at [0x10000,0x10040).
      // Bind only contained constant memory operands to one object. Original
      // integer addresses, instruction bodies and control flow stay intact.
      constexpr uint64_t Base = 0x10000, Bytes = 64;
      auto *Type = llvm::ArrayType::get(llvm::Type::getInt8Ty(Context), Bytes);
      auto *Stack = new llvm::GlobalVariable(*Module, Type, false,
                                             llvm::GlobalValue::ExternalLinkage,
                                             nullptr, "scheduled_test_stack");
      Stack->setAlignment(llvm::Align(64));
      unsigned Bound = 0;
      for (auto &Function : *Module)
        if (Function.getName().starts_with("proof_"))
          for (auto &Block : Function)
            for (auto &Instruction : Block) {
              const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
              const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
              if (!Load && !Store)
                continue;
              const unsigned Index = Load ? 0 : 1;
              const auto *Pointer = llvm::dyn_cast<llvm::ConstantExpr>(
                  Instruction.getOperand(Index));
              if (!Pointer ||
                  Pointer->getOpcode() != llvm::Instruction::IntToPtr)
                continue;
              const auto *Address =
                  llvm::dyn_cast<llvm::ConstantInt>(Pointer->getOperand(0));
              ASSERT_NE(Address, nullptr);
              const auto At = Address->getZExtValue();
              const auto Width =
                  Module->getDataLayout()
                      .getTypeStoreSize(
                          Load ? Load->getType()
                               : Store->getValueOperand()->getType())
                      .getFixedValue();
              ASSERT_GE(At, Base);
              ASSERT_LE(At, Base + Bytes);
              ASSERT_LE(Width, Base + Bytes - At);
              llvm::IRBuilder<> Builder(&Instruction);
              Instruction.setOperand(Index, Builder.CreateInBoundsGEP(
                                                Type, Stack,
                                                {Builder.getInt64(0),
                                                 Builder.getInt64(At - Base)}));
              ++Bound;
            }
      EXPECT_GT(Bound, 0u);
      InputIR = tmpFile(Name + ".bound.ll");
      std::string Mapped;
      llvm::raw_string_ostream Stream(Mapped);
      Module->print(Stream, nullptr);
      std::ofstream(InputIR) << Mapped;
    }
    auto Simplified =
        exec(NEVERD_TEST_CLANG,
             {"-O2", "-S", "-emit-llvm", InputIR.string(), "-o", IR.string()});
    if (const char *Root = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Root);
      for (const auto &Path : {File, InitialIR, InputIR, IR})
        if (fs::exists(Path))
          fs::copy_file(Path, fs::path(Root) / Path.filename(),
                        fs::copy_options::overwrite_existing);
    }
    ASSERT_TRUE(Simplified.ok()) << Simplified.err;
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    const auto Module = llvm::parseIRFile(IR.string(), Diagnostic, Context);
    ASSERT_NE(Module, nullptr);
    unsigned Count = 0;
    for (const auto &Function : *Module)
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
    EXPECT_EQ(Count, Paged ? 40u : 20u);
  }
};

TEST_F(X64ScheduledCallTest, AdmitsCheckedViewsAndOriginalCallSites) {
  loadCalls();
  for (bool Paged : {false, true})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Paged);
      SCOPED_TRACE(LLVM);
      const auto [Source, Report] = exportCalls(Paged, LLVM, false, false);
      ASSERT_FALSE(Source.empty());
      auto Manifest = llvm::json::parse(Report);
      ASSERT_TRUE(bool(Manifest));
      const auto *Calls = Manifest->getAsObject()->getObject("function_calls");
      ASSERT_NE(Calls, nullptr);
      EXPECT_EQ(Calls->getBoolean("scheduled_events"), true);
      const auto &Interface =
          *Calls->getArray("interfaces")->front().getAsObject();
      EXPECT_EQ(Interface.getString("call"),
                Paged ? "neverd_x64_call_paged_events_1020_v6"
                      : "neverd_x64_call_events_1020_v6");
      for (bool Pause : {false, true}) {
        const auto [Sites, SiteReport] = exportCalls(Paged, LLVM, true, Pause);
        ASSERT_FALSE(Sites.empty());
        auto SiteManifest = llvm::json::parse(SiteReport);
        ASSERT_TRUE(bool(SiteManifest));
        const auto *Contract =
            SiteManifest->getAsObject()->getObject("function_call_sites");
        ASSERT_NE(Contract, nullptr);
        EXPECT_EQ(Contract->getBoolean("scheduled_events"), true);
        EXPECT_EQ(Contract->getArray("interfaces")->size(), 2u);
        const auto File = tmpFile("scheduled-call-views.c");
        std::ofstream(File) << Sites;
        for (const char *Level : {"-O0", "-O2"}) {
          const auto Object =
              exec(NEVERD_TEST_CLANG,
                   {"-std=c11", Level, "-Werror=implicit-function-declaration",
                    "-Werror=incompatible-pointer-types", "-Werror=return-type",
                    "-c", File.string(), "-o",
                    tmpFile("scheduled-call-views.o").string()});
          ASSERT_TRUE(Object.ok()) << Object.err;
        }
      }
    }
}

TEST_F(X64ScheduledCallTest, RetainsArgumentAndProducerAdmission) {
  loadCalls();
  for (bool LLVM : {false, true}) {
    for (
        const auto &Bad :
        {request(
             R"("entries":[4128],"follow_internal_calls":true,"function_context":true,"scheduled_events":true,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[8],"result_bytes":4}])"),
         request(
             R"("entries":[4128],"follow_internal_calls":true,"function_context":true,"paged_memory":true,"scheduled_events":true,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[8,8,8,8,4],"result_bytes":4}])")}) {
      const char *Report = "old";
      EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Bad.data(),
                                                  Bad.size(), LLVM, &Report),
                nullptr);
      EXPECT_EQ(Report, nullptr);
      EXPECT_FALSE(take(neverd_last_error(Session)).empty());
    }
  }
}

TEST_F(X64ScheduledCallTest, ProvesOriginalCallsAndScheduledResumption) {
  loadCalls();
  for (bool Paged : {false, true})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Paged);
      SCOPED_TRACE(LLVM);
      const auto [Source, Report] = exportCalls(Paged, LLVM, true, true);
      ASSERT_FALSE(Source.empty());
      proveCalls(Paged, LLVM, Source);
    }
}
