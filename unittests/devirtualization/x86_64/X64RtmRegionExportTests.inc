//===- X64RtmRegionExportTests.inc - Complete native RTM attempts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare transaction commits, rollbacks and refusals with original bytes.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest, RtmRegionsOwnStateMemoryAndNestedControl) {
  // 0 commit, 1 abort, 2 nested commit, 3 nested abort, 4 loop, 5 CALL/RET,
  // 6 abort inside a callee, 7 CPUID abort, 8 PAUSE abort, 9 unsupported CLI,
  // 10 unselected indirect target, 11 attempt-budget exhaustion.
  for (unsigned Scenario = 0; Scenario < 12; ++Scenario) {
    SCOPED_TRACE(Scenario);
    std::vector<uint8_t> Bytes;
    std::vector<uint64_t> PCs;
    const auto Emit = [&](std::initializer_list<uint8_t> Code) {
      const size_t Offset = Bytes.size();
      PCs.push_back(0x1000 + Offset);
      Bytes.insert(Bytes.end(), Code);
      return Offset;
    };
    const auto Relative = [&](size_t At, size_t To, unsigned Width) {
      const uint64_t Delta = uint64_t(To) - (At + Width);
      for (unsigned I = 0; I < Width; ++I)
        Bytes[At + I] = uint8_t(Delta >> (8 * I));
    };
    const bool Short = Scenario & 1;
    if (Short)
      Emit({0x66, 0xc7, 0xf8, 0, 0});
    else
      Emit({0xc7, 0xf8, 0, 0, 0, 0});
    Emit({0x48, 0x89, 0x07}); // MOV [RDI], RAX.
    size_t Nested = 0, Call = 0;
    if (Scenario == 2 || Scenario == 3)
      Nested = Emit({0xc7, 0xf8, 0, 0, 0, 0});
    if (Scenario == 5 || Scenario == 6)
      Call = Emit({0xe8, 0, 0, 0, 0});
    else
      Emit({0xbb, 0x55, 0, 0, 0}); // MOV EBX, 0x55.
    if (Scenario == 4) {
      Emit({0xb9, 4, 0, 0, 0});    // MOV ECX, 4.
      Emit({0x48, 0x83, 0x07, 1}); // ADD qword [RDI], 1.
      Emit({0xff, 0xc9});          // DEC ECX.
      Emit({0x75, 0xf8});          // JNE to ADD.
    }
    if (Scenario == 7)
      Emit({0x0f, 0xa2});
    if (Scenario == 8)
      Emit({0xf3, 0x90});
    if (Scenario == 9)
      Emit({0xfa});
    if (Scenario == 10)
      Emit({0xff, 0xe6}); // JMP RSI, supplied outside the selection.
    if (Scenario == 11)
      Emit({0xeb, 0xfe});
    const auto Test = Emit({0x0f, 0x01, 0xd6}); // XTEST.
    if (Scenario == 1 || Scenario == 3)
      Emit({0xc6, 0xf8, 0xa7});
    if (Nested)
      Emit({0x0f, 0x01, 0xd5});
    const auto End = Emit({0x0f, 0x01, 0xd5});
    Emit({0xc3}); // Success continuation, outside the transaction.
    const auto Fallback = Emit({0xc3});
    // A distinct inner fallback must never receive a nested abort.
    const auto InnerFallback = Emit({0x0f, 0x0b});
    Relative(Short ? 3 : 2, Fallback, Short ? 2 : 4);
    if (Nested)
      Relative(Nested + 2, InnerFallback, 4);
    if (Call) {
      Relative(Call + 1, Bytes.size(), 4);
      Emit({0xbb, 0x55, 0, 0, 0});
      if (Scenario == 6)
        Emit({0xc6, 0xf8, 0xa7});
      Emit({0xc3});
    }
    // Original code zero must not collide with the private refusal channel.
    const auto StandaloneAbort = Emit({0xc6, 0xf8, 0});
    Emit({0xc3});
    load(Bytes);
    std::string Selection, Defines;
    llvm::raw_string_ostream S(Selection), D(Defines);
    S << "\"instructions\":[";
    for (size_t I = 0; I < PCs.size(); ++I)
      S << (I ? "," : "") << PCs[I];
    S << "],\"native_rtm_regions\":true,\"follow_internal_calls\":true";
    D << "\n#define SCENARIO " << Scenario << "\n#define COMMIT_PC "
      << 0x1000 + End + 3 << "\n#define FALLBACK_PC " << 0x1000 + Fallback
      << "\n#define CALL_RETURN " << 0x1000 + Call + 5 << "\n#define TEST_PC "
      << 0x1000 + Test << "\n#define END_PC " << 0x1000 + End
      << "\n#define ABORT_PC " << 0x1000 + StandaloneAbort
      << "\nstatic const unsigned char original_bytes[]={";
    for (size_t I = 0; I < Bytes.size(); ++I)
      D << (I ? "," : "") << unsigned(Bytes[I]);
    D << "};\n";
    const auto Harness = std::string(Seed) + Defines + RtmOriginalOracle + R"C(
int main(int argc, char **argv) {
  const int strict=argc==2, expected=strict ? atoi(argv[1]) : -1;
  unsigned a,b,c,d;
  const int supported=__get_cpuid_count(7,0,&a,&b,&c,&d) && (b&(1U<<11));
  if(strict)CHECK(supported==expected);
  void *code=mmap(0,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(code!=MAP_FAILED);memcpy(code,original_bytes,sizeof(original_bytes));
  for(unsigned pattern=0;pattern<64;++pattern) {
    uint64_t ram[3]={11,22,33},native_ram[3]={11,22,33},stack[5]={91,92,93,94,95};
    neverd_x64_state_v6 s=seed();
    s.GPR[0]+=pattern*17;s.GPR[7]=(uintptr_t)ram;s.GPR[4]=(uintptr_t)&stack[3];
    s.GPR[6]=0x77770000;
    const unsigned bits[]={0,2,4,6,7,11};s.RFlags=0x202|((pattern&1)<<10);
    for(unsigned i=0;i<6;++i)s.RFlags|=(uint64_t)((pattern>>i)&1)<<bits[i];
    neverd_x64_state_v6 before=s,want=s,native=s;
    neverd_x64_rtm_context_v6 context={SCENARIO==11 ? 8 : 128},saved=context;
    CHECK(!neverd_x64_rtm_environment_v6(&s,3,&context));
    CHECK(!neverd_x64_rtm_environment_v6(&s,4,0));
    CHECK(!neverd_x64_rtm_environment_v6(0,4,&context));
    CHECK(!memcmp(&s,&before,sizeof(s)));
    int handled=neverd_x64_rtm_environment_v6(&s,4,&context);
    CHECK(!memcmp(&context,&saved,sizeof(context)));
    if(!supported || !handled) {
      CHECK(!handled && !memcmp(&s,&before,sizeof(s)));
      CHECK(ram[0]==11 && stack[2]==93);
      if(strict && supported)CHECK(SCENARIO>=9);
    } else if(s.PC==FALLBACK_PC) {
      uint64_t status=s.GPR[0];CHECK(!(status>>32) && !(status&0x00ffffc0U));
      if(status&1)CHECK((status>>24)==0xa7 && !(status&2));
      else CHECK(!(status>>24));
      want.GPR[0]=status;want.PC=FALLBACK_PC;
      CHECK(!memcmp(&s,&want,sizeof(s)) && ram[0]==11 && stack[2]==93);
      if(strict)CHECK(SCENARIO==1 || SCENARIO==3 || SCENARIO==6 || SCENARIO==7 || SCENARIO==8);
    } else {
      CHECK(s.PC==COMMIT_PC);
      want.PC=COMMIT_PC;want.GPR[3]=0x55;want.RFlags&=~0x8d5ULL;
      if(SCENARIO==4)want.GPR[1]=0;
      CHECK(!memcmp(&s,&want,sizeof(s)));
      CHECK(ram[0]==before.GPR[0]+(SCENARIO==4 ? 4 : 0));
      CHECK(stack[2]==(SCENARIO==5 ? CALL_RETURN : 93));
      CHECK(SCENARIO==0 || SCENARIO==2 || SCENARIO==4 || SCENARIO==5);
    }
    CHECK(ram[1]==22 && ram[2]==33 && stack[0]==91 && stack[1]==92 && stack[3]==94 && stack[4]==95);
    if(strict && supported && SCENARIO<9) {
      original(&native,code,native_ram);
      CHECK(s.GPR[0]==native.GPR[0] && s.GPR[1]==native.GPR[1] && s.GPR[2]==native.GPR[2] && s.GPR[3]==native.GPR[3]);
      CHECK(s.RFlags==native.RFlags && !memcmp(ram,native_ram,sizeof(ram)));
    }
    uint64_t saved_ram[3],saved_stack[5];
    memcpy(saved_ram,ram,sizeof(ram));memcpy(saved_stack,stack,sizeof(stack));
    s=before;s.PC=TEST_PC;want=s;
    CHECK(neverd_x64_rtm_environment_v6(&s,4,&context)==supported);
    if(supported){want.RFlags=(want.RFlags&~0x8d5ULL)|0x40;want.PC+=3;}
    CHECK(!memcmp(&s,&want,sizeof(s)));
    s=before;s.PC=ABORT_PC;want=s;
    CHECK(neverd_x64_rtm_environment_v6(&s,4,&context)==supported);
    if(supported)want.PC+=3;
    CHECK(!memcmp(&s,&want,sizeof(s)));
    s=before;s.PC=END_PC;want=s;
    CHECK(!neverd_x64_rtm_environment_v6(&s,4,&context));
    CHECK(!memcmp(&s,&want,sizeof(s)));
    CHECK(!memcmp(ram,saved_ram,sizeof(ram)) && !memcmp(stack,saved_stack,sizeof(stack)));
    // Two instructions allow XBEGIN and the first store, then must roll both
    // back without publishing the provider's private abort status.
    s=before;want=s;ram[0]=11;stack[2]=93;context.max_instructions=2;
    handled=neverd_x64_rtm_environment_v6(&s,4,&context);
    if(strict)CHECK(!handled);
    if(handled) {
      CHECK(s.PC==FALLBACK_PC && !(s.GPR[0]&1) && !(s.GPR[0]&~0x3eULL));
      want.PC=FALLBACK_PC;want.GPR[0]=s.GPR[0];
    }
    CHECK(!memcmp(&s,&want,sizeof(s)) && ram[0]==11 && stack[2]==93);
    s=before;context.max_instructions=0;
    CHECK(!neverd_x64_rtm_environment_v6(&s,4,&context) && !memcmp(&s,&before,sizeof(s)));
  }
  CHECK(!munmap(code,4096));
  return 0;
}
)C";
    for (bool LLVM : {false, true})
      for (const char *Dispatch : {"switch", "direct"}) {
        auto [Code, Report] = source(
            request(Selection + ",\"dispatch\":\"" + Dispatch + "\""), LLVM);
        auto Parsed = llvm::json::parse(Report);
        ASSERT_TRUE(bool(Parsed));
        auto *Binding =
            Parsed->getAsObject()->getObject("native_rtm_environment");
        ASSERT_NE(Binding, nullptr);
        EXPECT_EQ(Binding->getArray("begins")->size(), Nested ? 2u : 1u);
        const auto *Body = Binding->getArray("body_steps");
        ASSERT_NE(Body, nullptr);
        EXPECT_FALSE(Body->empty());
        execute(Code, Harness);
        if (const char *Directory =
                std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
          const auto Base = std::string(Directory) + "/rtm-region-" +
                            std::to_string(Scenario) +
                            (LLVM ? "-llvm-" : "-high-") + Dispatch;
          std::ofstream(Base + ".c") << Code;
          std::ofstream(Base + ".json") << Report;
          std::ofstream(Base + ".test.c") << Code << Harness;
        }
      }
  }
}

TEST_F(X64ContinuationExportTest,
       RtmRegionsDiscoverBothBeginExitsAndEndResume) {
  load({0xc7, 0xf8, 8, 0, 0, 0, 0xbb, 0x55, 0, 0, 0, 0x0f, 0x01, 0xd5, 0xc3},
       {{0x1000, 15}});
  for (bool LLVM : {false, true}) {
    auto [Code, Report] =
        source(request(R"("entries":[4096],"native_rtm_regions":true)"), LLVM);
    auto Parsed = llvm::json::parse(Report);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("total"), 4);
    auto *Binding = Parsed->getAsObject()->getObject("native_rtm_environment");
    ASSERT_NE(Binding, nullptr);
    EXPECT_EQ(Binding->getArray("begins")->size(), 1u);
    EXPECT_EQ(Binding->getArray("body_steps")->size(), 2u);
  }
}

TEST_F(X64ContinuationExportTest, RtmRegionsDoNotAdmitVirtualSystemEffects) {
  // MOV CR8,RAX; CLI; STD; CLD; MOV EBX,85. Ordinary source support does
  // not grant transaction admission for virtualized control state or DF.
  load({0x44, 0x0f, 0x22, 0xc0, 0xfa, 0xfd, 0xfc, 0xbb, 0x55, 0, 0, 0});
  auto [Code, Report] = source(
      request(
          R"("instructions":[4096,4100,4101,4102,4103],"native_rtm_regions":true)"),
      false);
  auto Parsed = llvm::json::parse(Report);
  ASSERT_TRUE(bool(Parsed));
  const auto *Units = Parsed->getAsObject()->getArray("units");
  ASSERT_NE(Units, nullptr);
  ASSERT_EQ(Units->size(), 5u);
  for (unsigned I = 0; I < 5; ++I)
    EXPECT_EQ((*Units)[I].getAsObject()->getBoolean("rtm_body"), I == 4);
}

TEST_F(X64ContinuationExportTest,
       RtmRegionsNeverConfuseOriginalAbortWithRefusal) {
  std::vector<uint8_t> Bytes;
  std::string Addresses;
  for (unsigned Code = 0; Code < 256; ++Code) {
    if (Code)
      Addresses += ",";
    Addresses += std::to_string(0x1000 + Bytes.size());
    if (Code & 1)
      Bytes.push_back(0x66);
    Bytes.insert(Bytes.end(), {0xc6, 0xf8, uint8_t(Code)});
  }
  load(Bytes);
  const auto Request = request("\"instructions\":[" + Addresses +
                               "],\"native_rtm_regions\":true");
  const char *Report = nullptr;
  EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Request.data(),
                                              Request.size(), false, &Report),
            nullptr);
  EXPECT_EQ(Report, nullptr);
  EXPECT_NE(take(neverd_last_error(Session)).find("all 256 codes"),
            std::string::npos);
}
