//===- X64StackObserverTests.inc - Original stack observer proofs -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve private stack bytes and spilled pointers across native CALL views.
/// Only source export, compilation and LLVM constant proofs are performed.
//===----------------------------------------------------------------------===//

#include "X64StackObserverProof.inc"

class X64StackObserverTest : public X64ScheduledCallTest {
protected:
  void loadObserver() {
    // The caller overwrites only the low DWORD of its private slot. The
    // observer obtains the slot pointer from a saved RDI cell, then reads the
    // upper DWORD. Neither an ordinary scalar argument nor a copied start
    // value can replace the original storage and its aliases.
    const uint8_t Caller[] = {0x57,                         // PUSH RDI
                              0x48, 0x83, 0xec, 0x30,       // SUB RSP, 48
                              0x89, 0x4c, 0x24, 0x20,       // MOV [RSP+32], ECX
                              0x48, 0x8d, 0x7c, 0x24, 0x20, // LEA RDI, [RSP+32]
                              0xe8, 0x0d, 0,    0,    0,    // CALL 0x1020
                              0x48, 0x83, 0xc4, 0x30,       // ADD RSP, 48
                              0x5f, 0xc3};                  // POP RDI; RET
    const uint8_t Observer[] = {0x57,                       // PUSH RDI
                                0x48, 0x83, 0xec, 0x20,     // SUB RSP, 32
                                0x48, 0x8b, 0x44, 0x24,
                                0x20,                   // MOV RAX, [RSP+32]
                                0x8b, 0x40, 0x04,       // MOV EAX, [RAX+4]
                                0x48, 0x83, 0xc4, 0x20, // ADD RSP, 32
                                0x5f, 0xc3};            // POP RDI; RET
    std::vector<uint8_t> Code(32 + sizeof(Observer), 0x90);
    std::copy(std::begin(Caller), std::end(Caller), Code.begin());
    std::copy(std::begin(Observer), std::end(Observer), Code.begin() + 32);
    load(Code, {{0x1000, sizeof(Caller)}, {0x1020, sizeof(Observer)}});
  }

  std::string exportObserver(bool Paged, bool Debug, bool Scheduled,
                             bool LLVM) {
    const auto Request = request(
        (R"("entries":[4096,4128],"follow_internal_calls":true,"function_context":true,"pause_call_sites":true,"paged_memory":)" +
         llvm::Twine(Paged ? "true" : "false") + R"(,"paged_debug":)" +
         (Debug ? "true" : "false") + R"(,"scheduled_events":)" +
         (Scheduled ? "true" : "false") +
         (Paged ? "" : R"(,"event_delivery":true)") +
         R"(,"function_calls":[{"entry":4096,"abi":"win64","argument_bytes":[4],"result_bytes":4},{"entry":4128,"abi":"win64","argument_bytes":[],"result_bytes":4,"call_sites":[4110]}])")
            .str());
    const char *Report = nullptr;
    auto Source = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), LLVM, &Report));
    EXPECT_FALSE(Source.empty()) << take(neverd_last_error(Session));
    auto Manifest = llvm::json::parse(take(Report));
    EXPECT_TRUE(bool(Manifest));
    if (Manifest) {
      EXPECT_EQ(Manifest->getAsObject()->getInteger("modeled"), 15);
      EXPECT_EQ(Manifest->getAsObject()->getInteger("native_requests"), 0);
    }
    return Source;
  }
};

TEST_F(X64StackObserverTest, CompileAndProveOriginalMemoryObservers) {
  if (!neverd::test::compilerReadsCurrentIR(NEVERD_TEST_CLANG))
    GTEST_SKIP() << "constant predicates require the current optimizer";
  loadObserver();
  for (unsigned Mode = 0; Mode != 3; ++Mode)
    for (bool Scheduled : {false, true})
      for (bool LLVM : {false, true}) {
        const bool Paged = Mode != 0, Debug = Mode == 2;
        const auto Name = "stack-observer-" + std::to_string(Mode) +
                          (Scheduled ? "-events" : "") +
                          (LLVM ? "-llvm" : "-high");
        SCOPED_TRACE(Name);
        const auto Source = exportObserver(Paged, Debug, Scheduled, LLVM);
        ASSERT_FALSE(Source.empty());
        const auto File = tmpFile(Name + "-unmodified.c");
        std::ofstream(File) << Source;
        for (const char *Level : {"-O0", "-O2"}) {
          const auto Built = exec(NEVERD_TEST_CLANG,
                                  {"-std=c11", Level, "-Werror=uninitialized",
                                   "-Werror=return-type", "-c", File.string(),
                                   "-o", tmpFile(Name + ".o").string()});
          ASSERT_TRUE(Built.ok()) << Built.err;
        }
        const auto Prefix = std::string(Paged ? "paged_" : "") +
                            (Debug ? "debug_" : "") +
                            (Scheduled ? "events_" : "");
        const auto Version = Debug ? "2" : "1";
        std::string Probes;
        llvm::raw_string_ostream OS(Probes);
        OS << "\n#define PAGED " << Paged << "\n#define DEBUG " << Debug
           << "\n#define SCHEDULED " << Scheduled
           << "\n#define CALL_CONTEXT neverd_x64_call_context_v" << Version
           << "\n#define SITE_CONTEXT neverd_x64_call_site_context_v" << Version
           << "\n#define START_ROOT neverd_x64_call_" << Prefix << "1000_v6"
           << "\n#define RESUME_ROOT neverd_x64_resume_" << Prefix << "1000_v6"
           << "\n#define START_SITE neverd_x64_call_" << Prefix
           << "site_100E_v6"
           << "\n#define RESUME_SITE neverd_x64_resume_" << Prefix
           << "site_100E_v6\n"
           << StackObserverProbes;
        provePredicates(Paged, Name, Source, Probes, 4, 256, 4, true);
      }
}
