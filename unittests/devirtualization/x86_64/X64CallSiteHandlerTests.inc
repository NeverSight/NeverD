//===- X64CallSiteHandlerTests.inc - Pending CALL handler composition ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile selected-handler continuation through both C routes. No emitted
/// program, specimen, instruction executor or guest is executed.
//===----------------------------------------------------------------------===//

#include "X64CallSiteHandlerProof.inc"

class X64CallSiteHandlerTest : public X64ScheduledCallTest {
protected:
  void loadHandler() {
    std::vector<uint8_t> Code(53, 0x90);
    const uint8_t Caller[] = {0xb9, 0x44, 0,    0,    0,    0xe8, 0x16, 0,
                              0,    0,    0x48, 0x83, 0xec, 0x10, 0xc3};
    const uint8_t Other[] = {0xe8, 0x0b, 0, 0, 0, 0xc3};
    const uint8_t Callee[] = {0x8b, 0x44, 0x24, 0x2c, 0x31,
                              0xd0, 0xc2, 0x10, 0};
    // The handler changes RDX through the actual MOV, then returns through the
    // original IRETQ. The callee reads the original fifth slot's upper DWORD.
    const uint8_t Handler[] = {0x44, 0x89, 0xda, 0x48, 0xcf};
    std::copy(std::begin(Caller), std::end(Caller), Code.begin());
    std::copy(std::begin(Other), std::end(Other), Code.begin() + 16);
    std::copy(std::begin(Callee), std::end(Callee), Code.begin() + 32);
    std::copy(std::begin(Handler), std::end(Handler), Code.begin() + 48);
    load(Code, {{0x1000, 15}, {0x1010, 6}, {0x1020, 9}, {0x1030, 5}});
  }

  std::pair<std::string, std::string> exportHandlers(bool Paged, bool Scheduled,
                                                     bool LLVM, bool Direct) {
    const auto Request = request(
        (R"("entries":[4096,4112,4128,4144],"follow_internal_calls":true,"function_context":true,"pause_call_sites":true,"paged_memory":)" +
         llvm::Twine(Paged ? "true" : "false") +
         (Paged ? "" : R"(,"event_delivery":true)") +
         R"(,"scheduled_events":)" + (Scheduled ? "true" : "false") +
         (Direct ? R"(,"dispatch":"direct")" : "") +
         R"(,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[1,null,4,8,null,null],"result_bytes":4,"call_sites":[4101,4112]}])")
            .str());
    const char *Report = nullptr;
    auto Source = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), LLVM, &Report));
    EXPECT_FALSE(Source.empty()) << take(neverd_last_error(Session));
    return {std::move(Source), take(Report)};
  }
};

TEST_F(X64CallSiteHandlerTest, CompileOnlyAndManifest) {
  loadHandler();
  for (bool Paged : {false, true})
    for (bool Scheduled : {false, true})
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(Paged);
        SCOPED_TRACE(Scheduled);
        SCOPED_TRACE(LLVM);
        const auto [Source, Report] =
            exportHandlers(Paged, Scheduled, LLVM, false);
        ASSERT_FALSE(Source.empty());
        auto Manifest = llvm::json::parse(Report);
        ASSERT_TRUE(bool(Manifest));
        const auto *Sites =
            Manifest->getAsObject()->getObject("function_call_sites");
        ASSERT_NE(Sites, nullptr);
        EXPECT_EQ(Sites->getString("handler_context_type"),
                  "neverd_x64_call_site_handler_context_v1");
        EXPECT_EQ(Sites->getString("begin_handler"),
                  "neverd_x64_begin_call_site_handler_v1");
        const auto *Interfaces = Sites->getArray("interfaces");
        ASSERT_NE(Interfaces, nullptr);
        ASSERT_EQ(Interfaces->size(), 2u);
        for (const auto &Interface : *Interfaces) {
          const auto Name =
              Interface.getAsObject()->getString("resume_handler");
          ASSERT_TRUE(Name.has_value());
          EXPECT_NE(Source.find(Name->str()), std::string::npos);
        }
        const auto File = tmpFile("call-handler-compile.c");
        std::ofstream(File) << Source;
        for (const char *Level : {"-O0", "-O2"}) {
          const auto Built = exec(NEVERD_TEST_CLANG,
                                  {"-std=c11", Level, "-Werror=uninitialized",
                                   "-Werror=return-type", "-c", File.string(),
                                   "-o", tmpFile("call-handler.o").string()});
          ASSERT_TRUE(Built.ok()) << Built.err;
        }
      }
}

TEST_F(X64CallSiteHandlerTest, ProvesReturnsAndPendingRetries) {
  if (!neverd::test::compilerReadsCurrentIR(NEVERD_TEST_CLANG))
    GTEST_SKIP() << "constant predicates require the current optimizer";
  loadHandler();
  for (bool Paged : {false, true})
    for (bool Scheduled : {false, true})
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(Paged);
        SCOPED_TRACE(Scheduled);
        SCOPED_TRACE(LLVM);
        const auto [Source, Report] =
            exportHandlers(Paged, Scheduled, LLVM, false);
        ASSERT_FALSE(Source.empty());
        const auto Name =
            std::string("call-handler-") + (Paged ? "paged-" : "plain-") +
            (Scheduled ? "events-" : "") + (LLVM ? "llvm" : "high");
        const auto Probes = "\n#define PAGED " + std::to_string(Paged) +
                            "\n#define SCHEDULED " + std::to_string(Scheduled) +
                            "\n" + CallSiteHandlerProbes;
        provePredicates(Paged, Name, Source, Probes,
                        14 + unsigned(Scheduled) + (Paged ? 5 : 0), 0x800);
      }
}

TEST_F(X64CallSiteHandlerTest, DirectDriverUsesTheSameReturnBoundary) {
  if (!neverd::test::compilerReadsCurrentIR(NEVERD_TEST_CLANG))
    GTEST_SKIP() << "constant predicates require the current optimizer";
  loadHandler();
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto [Source, Report] = exportHandlers(false, false, LLVM, true);
    ASSERT_FALSE(Source.empty());
    provePredicates(
        false, LLVM ? "call-handler-direct-llvm" : "call-handler-direct-high",
        Source,
        std::string("\n#define PAGED 0\n#define SCHEDULED 0\n") +
            CallSiteHandlerProbes,
        14, 0x800);
  }
}
