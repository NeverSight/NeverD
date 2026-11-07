//===- X64JumpRegionContinuationTests.inc - Direct jump discovery --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by X64ContinuationExportTests.cpp inside its test namespace.
/// Select original jump targets without ordinary tail-call or frame
/// assumptions.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest,
       DirectJumpDiscoveryRetainsCallDepthAndPhysicalArguments) {
  std::vector<uint8_t> Code(0x77, 0x90);
  const uint8_t Entry[] = {0xe9, 0x3b, 0, 0, 0};  // jmp 1040
  const uint8_t Middle[] = {0xe8, 0x1b, 0, 0, 0,  // call 1060
                            0xe9, 0x26, 0, 0, 0}; // jmp 1070
  const uint8_t Callee[] = {0x48, 0x83, 0x44, 0x24, 0x38, 3, 0xc3};
  const uint8_t Tail[] = {0x48, 0x8b, 0x44, 0x24, 0x30, 0xff, 0xe2};
  std::copy(std::begin(Entry), std::end(Entry), Code.begin());
  std::copy(std::begin(Middle), std::end(Middle), Code.begin() + 0x40);
  std::copy(std::begin(Callee), std::end(Callee), Code.begin() + 0x60);
  std::copy(std::begin(Tail), std::end(Tail), Code.begin() + 0x70);
  load(Code, {{0x1000, 5}, {0x1040, 10}, {0x1060, 7}, {0x1070, 7}});
  for (bool LLVM : {false, true})
    for (bool Direct : {false, true})
      for (unsigned Depth : {0u, 1u}) {
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(Direct);
        SCOPED_TRACE(Depth);
        const std::string Selection =
            R"("entries":[4096],"follow_internal_calls":true,"callee_depth":)" +
            std::to_string(Depth) +
            ",\"dispatch\":" + (Direct ? "\"direct\"" : "\"switch\"");
        auto [Source, Manifest] =
            source(request(Selection + R"(,"follow_direct_jumps":true)"), LLVM);
        auto Parsed = llvm::json::parse(Manifest);
        ASSERT_TRUE(bool(Parsed));
        const auto *Regions = Parsed->getAsObject()->getArray("regions");
        ASSERT_NE(Regions, nullptr);
        EXPECT_EQ(Regions->size(), Depth ? 4u : 3u);
        EXPECT_EQ(Parsed->getAsObject()->getInteger("total"), Depth ? 7 : 5);
        EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
        for (size_t I = 0; I < Regions->size(); ++I) {
          const auto *Region = (*Regions)[I].getAsObject();
          if (Depth)
            EXPECT_EQ(Region->getInteger("callee_depth"), I == 3 ? 1 : 0);
          if (I < 2) {
            const auto *Jumps = Region->getArray("direct_jump_successors");
            ASSERT_NE(Jumps, nullptr);
            ASSERT_EQ(Jumps->size(), 1u);
            EXPECT_EQ(Jumps->front().getAsObject()->getString("instruction"),
                      I ? "0x1045" : "0x1000");
            EXPECT_EQ(Jumps->front().getAsObject()->getString("target"),
                      I ? "0x1070" : "0x1040");
          }
        }
        execute(Source, "#define DEPTH " + std::to_string(Depth) + Seed + R"C(
int main(void) {
  for(unsigned i=0;i<64;++i) {
    uint64_t stack[32],want[32];
    for(unsigned j=0;j<32;++j)stack[j]=0x1234567800000000ULL+i*71+j;
    stack[8]=0x7777;memcpy(want,stack,sizeof(stack));
    want[7]=0x1045;want[14]+=3;
    neverd_x64_state_v6 s=seed();s.GPR[4]=(uintptr_t)&stack[8];s.GPR[2]=0x7777;
    uint64_t budget=8;
    if(!DEPTH) {
      CHECK(neverd_x64_continue_v6(&s,&budget)==1 && budget==6);
      CHECK(s.PC==0x1060 && s.GPR[4]==(uintptr_t)&stack[7]);
      // The frontier remains an environment-owned call. Reentry takes its
      // actual saved PC and stack mutation, not an inferred lexical return.
      stack[14]+=3;s.PC=stack[7];s.GPR[4]+=8;
    }
    CHECK(neverd_x64_continue_v6(&s,&budget)==2 && budget==(DEPTH?1:3));
    CHECK(s.PC==0x7777 && s.GPR[4]==(uintptr_t)&stack[8]);
    CHECK(s.GPR[0]==want[14] && !memcmp(stack,want,sizeof(stack)));
  }
  return 0;
}
)C");
        auto [Default, DefaultManifest] = source(request(Selection), LLVM);
        auto [Disabled, DisabledManifest] = source(
            request(Selection + R"(,"follow_direct_jumps":false)"), LLVM);
        EXPECT_EQ(Disabled, Default);
        execute(Default, std::string(Seed) + R"C(
int main(void) {
  neverd_x64_state_v6 s=seed();uint64_t budget=7;
  CHECK(neverd_x64_continue_v6(&s,&budget)==2 && budget==6 && s.PC==0x1040);
  return 0;
}
)C");
      }
  const auto TooSmall = request(
      R"("entries":[4096],"callee_depth":1,"follow_internal_calls":true,"follow_direct_jumps":true,"max_instructions":6)");
  const char *Report = "old";
  EXPECT_EQ(neverd_x64_continuation_source_v1(Session, TooSmall.data(),
                                              TooSmall.size(), 0, &Report),
            nullptr);
  EXPECT_EQ(Report, nullptr);
}

TEST_F(X64ContinuationExportTest,
       DirectJumpDiscoveryDeduplicatesCyclesAndKeepsIndirectBoundaries) {
  std::vector<uint8_t> Code(0x25, 0x90);
  const uint8_t Forward[] = {0xe9, 0x1b, 0, 0, 0};
  const uint8_t Backward[] = {0xe9, 0xdb, 0xff, 0xff, 0xff};
  std::copy(std::begin(Forward), std::end(Forward), Code.begin());
  std::copy(std::begin(Backward), std::end(Backward), Code.begin() + 0x20);
  load(Code, {{0x1000, 5}, {0x1020, 5}});
  const auto Request = request(
      R"("entries":[4096],"follow_direct_jumps":true,"dispatch":"direct")");
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(Request, LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("total"), 2);
    EXPECT_EQ(Parsed->getAsObject()->getArray("regions")->size(), 2u);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
  neverd_x64_state_v6 s=seed();uint64_t budget=17;
  CHECK(neverd_x64_continue_v6(&s,&budget)==9 && !budget && s.PC==0x1020);
  return 0;
}
)C");
  }
  // A runtime target does not supply a direct discovery edge.
  load({0xff, 0xe0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xc3});
  auto [Source, Manifest] = source(Request, false);
  auto Parsed = llvm::json::parse(Manifest);
  ASSERT_TRUE(bool(Parsed));
  EXPECT_EQ(Parsed->getAsObject()->getArray("regions")->size(), 1u);
  execute(Source, std::string(Seed) + R"C(
int main(void) {
  neverd_x64_state_v6 s=seed();s.GPR[0]=0x1008;uint64_t budget=3;
  CHECK(neverd_x64_continue_v6(&s,&budget)==2 && budget==2 && s.PC==0x1008);
  return 0;
}
)C");
}
