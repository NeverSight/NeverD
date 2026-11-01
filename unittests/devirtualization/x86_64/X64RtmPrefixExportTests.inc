//===- X64RtmPrefixExportTests.inc - Original RTM prefix execution -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute every prefix family through the shared native transaction owner.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest,
       RtmRegionsRetainPrefixedControlsAndDiscovery) {
  const auto Prefixes = x86TransactionTestPrefixes(true);
  std::vector<uint8_t> Bytes;
  std::string Defines, Entries;
  llvm::raw_string_ostream D(Defines), E(Entries);
  D << "\nstatic const struct { uint64_t entry, commit, fallback, test, abort, "
       "end; "
       "unsigned control_size, abort_size, code; } cases[]={\n";
  E << "\"entries\":[";
  for (size_t I = 0; I < Prefixes.size(); ++I) {
    const auto &[Prefix, Width] = Prefixes[I];
    const auto Root = Bytes.size();
    E << (I ? "," : "") << 0x1000 + Root;
    const auto Emit = [&](std::initializer_list<uint8_t> Code, bool Prefixed) {
      const auto Offset = Bytes.size();
      if (Prefixed)
        for (uint8_t Byte : Prefix)
          // XEND/XTEST have NP encodings, unlike XBEGIN/XABORT. Invalid
          // prefixes are tested separately; never normalize them at runtime.
          if (*Code.begin() != 0x0f ||
              (Byte != 0x66 && Byte != 0xf2 && Byte != 0xf3))
            Bytes.push_back(Byte);
      Bytes.insert(Bytes.end(), Code);
      return Offset;
    };
    Emit({0xc7, 0xf8}, true);
    const auto Relative = Bytes.size();
    Bytes.insert(Bytes.end(), Width, 0);
    Emit({0x48, 0x89, 0x07}, false); // MOV [RDI],RAX.
    const auto Test = Emit({0x0f, 0x01, 0xd6}, true);
    const auto Branch =
        Emit({0xe3, 0}, false); // JRCXZ to commit, no flag changes.
    // Include original code zero with ignored prefixes: it cannot be mistaken
    // for the private rollback channel. Other cases cover both signed halves.
    const unsigned Code = I == 0 ? 0xff : unsigned((I - 1) * 11) & 0xff;
    const auto Abort = Emit({0xc6, 0xf8, uint8_t(Code)}, true);
    const auto End = Emit({0x0f, 0x01, 0xd5}, true);
    const auto Commit = Emit({0xc3}, false);
    const auto Fallback = Emit({0xc3}, false);
    const uint64_t Delta = Fallback - (Relative + Width);
    for (unsigned N = 0; N < Width; ++N)
      Bytes[Relative + N] = uint8_t(Delta >> (8 * N));
    Bytes[Branch + 1] = uint8_t(End - (Branch + 2));
    D << "{" << 0x1000 + Root << "," << 0x1000 + Commit << ","
      << 0x1000 + Fallback << "," << 0x1000 + Test << "," << 0x1000 + Abort
      << "," << 0x1000 + End << "," << Branch - Test << "," << Prefix.size() + 3
      << "," << Code << "},\n";
  }
  E << "],\"native_rtm_regions\":true";
  D << "};\nstatic const unsigned char original_bytes[]={";
  for (size_t I = 0; I < Bytes.size(); ++I)
    D << (I ? "," : "") << unsigned(Bytes[I]);
  D << "};\n";
  load(Bytes);
  const auto Harness = std::string(Seed) + Defines + RtmOriginalOracle + R"C(
int main(int argc, char **argv) {
  const int strict=argc==2,expected=strict ? atoi(argv[1]) : -1;
  unsigned a,b,c,d;
  const int supported=__get_cpuid_count(7,0,&a,&b,&c,&d) && (b&(1U<<11));
  if(strict)CHECK(supported==expected);
  void *code=mmap(0,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(code!=MAP_FAILED && sizeof(original_bytes)<=4096);
  memcpy(code,original_bytes,sizeof(original_bytes));
  for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i)
    for(unsigned pattern=0;pattern<64;++pattern)
      for(unsigned aborting=0;aborting<2;++aborting) {
        uint64_t ram[3]={11,22,33},native_ram[3]={11,22,33};
        neverd_x64_state_v6 s=seed();
        s.PC=cases[i].entry;s.GPR[0]+=pattern*17;
        s.GPR[1]=aborting;s.GPR[7]=(uintptr_t)ram;
        const unsigned bits[]={0,2,4,6,7,11};s.RFlags=0x202|((pattern&1)<<10);
        for(unsigned n=0;n<6;++n)s.RFlags|=(uint64_t)((pattern>>n)&1)<<bits[n];
        neverd_x64_state_v6 before=s,want=s,native=s;
        neverd_x64_rtm_context_v6 context={16};
        int handled=neverd_x64_rtm_environment_v6(&s,4,&context);
        if(!supported) {
          CHECK(!handled && !memcmp(&s,&before,sizeof(s)) && ram[0]==11);
        } else {
          CHECK(handled);
          if(s.PC==cases[i].fallback) {
            uint64_t status=s.GPR[0];
            CHECK(!(status>>32) && !(status&0x00ffffc0U));
            if(status&1)CHECK((status>>24)==cases[i].code && !(status&2));
            else CHECK(!(status>>24));
            want.GPR[0]=status;want.PC=cases[i].fallback;
            CHECK(ram[0]==11);
            if(strict)CHECK(aborting);
          } else {
            CHECK(!aborting && s.PC==cases[i].commit);
            want.PC=cases[i].commit;want.RFlags&=~0x8d5ULL;
            CHECK(ram[0]==before.GPR[0]);
          }
          CHECK(!memcmp(&s,&want,sizeof(s)));
          if(strict) {
            original(&native,(char *)code+cases[i].entry-0x1000,native_ram);
            CHECK(s.GPR[0]==native.GPR[0] && s.GPR[1]==native.GPR[1]);
            CHECK(s.GPR[2]==native.GPR[2] && s.GPR[3]==native.GPR[3]);
            CHECK(s.RFlags==native.RFlags && !memcmp(ram,native_ram,sizeof(ram)));
          }
        }
        CHECK(ram[1]==22 && ram[2]==33);
        s=before;s.PC=cases[i].test;want=s;
        CHECK(neverd_x64_rtm_environment_v6(&s,4,&context)==supported);
        if(supported){want.RFlags=(want.RFlags&~0x8d5ULL)|0x40;want.PC+=cases[i].control_size;}
        CHECK(!memcmp(&s,&want,sizeof(s)));
        s=before;s.PC=cases[i].abort;want=s;
        CHECK(neverd_x64_rtm_environment_v6(&s,4,&context)==supported);
        if(supported)want.PC+=cases[i].abort_size;
        CHECK(!memcmp(&s,&want,sizeof(s)));
        s=before;s.PC=cases[i].end;want=s;
        CHECK(!neverd_x64_rtm_environment_v6(&s,4,&context));
        CHECK(!memcmp(&s,&want,sizeof(s)));
      }
  CHECK(!munmap(code,4096));
  return 0;
}
)C";
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      auto [Code, Report] =
          source(request(Entries + ",\"dispatch\":\"" + Dispatch + "\""), LLVM);
      auto Parsed = llvm::json::parse(Report);
      ASSERT_TRUE(bool(Parsed));
      const auto *Binding =
          Parsed->getAsObject()->getObject("native_rtm_environment");
      ASSERT_NE(Binding, nullptr);
      EXPECT_EQ(Binding->getArray("begins")->size(), Prefixes.size());
      EXPECT_EQ(Binding->getArray("standalone_controls")->size(),
                Prefixes.size() * 2);
      execute(Code, Harness);
      if (const char *Directory =
              std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
        const auto Base = std::string(Directory) + "/rtm-prefix-" +
                          (LLVM ? "llvm-" : "high-") + Dispatch;
        std::ofstream(Base + ".c") << Code;
        std::ofstream(Base + ".json") << Report;
        std::ofstream(Base + ".test.c") << Code << Harness;
      }
    }
  RecordProperty("prefix_cases", Prefixes.size());
}

TEST_F(X64ContinuationExportTest,
       RtmAbortRegionsRetainBothInstructionPrefixes) {
  const auto Prefixes = x86TransactionTestPrefixes(true);
  std::vector<uint8_t> Bytes;
  std::string Defines, Entries;
  llvm::raw_string_ostream D(Defines), E(Entries);
  D << "\nstatic const struct { uint64_t entry, fallback; unsigned code; } "
       "cases[]={\n";
  E << "\"entries\":[";
  for (size_t I = 0; I < Prefixes.size(); ++I) {
    const auto &[Prefix, Width] = Prefixes[I];
    const auto &AbortPrefix = Prefixes[Prefixes.size() - I - 1].first;
    const auto Root = Bytes.size();
    E << (I ? "," : "") << 0x1000 + Root;
    Bytes.insert(Bytes.end(), Prefix.begin(), Prefix.end());
    Bytes.insert(Bytes.end(), {0xc7, 0xf8});
    const uint64_t Delta = AbortPrefix.size() + 3;
    for (unsigned N = 0; N < Width; ++N)
      Bytes.push_back(uint8_t(Delta >> (8 * N)));
    Bytes.insert(Bytes.end(), AbortPrefix.begin(), AbortPrefix.end());
    const unsigned Code = (I * 11) & 0xff;
    Bytes.insert(Bytes.end(), {0xc6, 0xf8, uint8_t(Code)});
    D << "{" << 0x1000 + Root << "," << 0x1000 + Bytes.size() << "," << Code
      << "},\n";
    Bytes.push_back(0xc3);
  }
  E << "],\"native_rtm_abort_regions\":true";
  D << "};\nstatic const unsigned char original_bytes[]={";
  for (size_t I = 0; I < Bytes.size(); ++I)
    D << (I ? "," : "") << unsigned(Bytes[I]);
  D << "};\n";
  load(Bytes);
  const auto Harness = std::string(Seed) + Defines + RtmOriginalOracle + R"C(
int main(int argc, char **argv) {
  const int strict=argc==2,expected=strict ? atoi(argv[1]) : -1;
  unsigned a,b,c,d;
  const int supported=__get_cpuid_count(7,0,&a,&b,&c,&d) && (b&(1U<<11));
  if(strict)CHECK(supported==expected);
  void *code=mmap(0,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(code!=MAP_FAILED && sizeof(original_bytes)<=4096);
  memcpy(code,original_bytes,sizeof(original_bytes));
  for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i)
    for(unsigned pattern=0;pattern<64;++pattern) {
      uint64_t ram[3]={11,22,33};
      neverd_x64_state_v6 s=seed();
      s.PC=cases[i].entry;s.GPR[0]+=pattern*17;s.GPR[7]=(uintptr_t)ram;
      const unsigned bits[]={0,2,4,6,7,11};s.RFlags=0x202|((pattern&1)<<10);
      for(unsigned n=0;n<6;++n)s.RFlags|=(uint64_t)((pattern>>n)&1)<<bits[n];
      neverd_x64_state_v6 before=s,want=s,native=s;
      CHECK(neverd_x64_rtm_abort_environment_v6(&s,4,0)==supported);
      if(supported) {
        uint64_t status=s.GPR[0];CHECK(!(status>>32) && !(status&0x00ffffc0U));
        if(status&1)CHECK((status>>24)==cases[i].code && !(status&2));
        else CHECK(!(status>>24));
        want.GPR[0]=status;want.PC=cases[i].fallback;
        if(strict) {
          original(&native,(char *)code+cases[i].entry-0x1000,ram);
          CHECK(status==native.GPR[0] && s.RFlags==native.RFlags);
          CHECK(s.GPR[1]==native.GPR[1] && s.GPR[2]==native.GPR[2] && s.GPR[3]==native.GPR[3]);
        }
      }
      CHECK(!memcmp(&s,&want,sizeof(s)));
      CHECK(ram[0]==11 && ram[1]==22 && ram[2]==33);
    }
  CHECK(!munmap(code,4096));
  return 0;
}
)C";
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      auto [Code, Report] =
          source(request(Entries + ",\"dispatch\":\"" + Dispatch + "\""), LLVM);
      auto Parsed = llvm::json::parse(Report);
      ASSERT_TRUE(bool(Parsed));
      const auto *Binding =
          Parsed->getAsObject()->getObject("native_rtm_abort_environment");
      ASSERT_NE(Binding, nullptr);
      EXPECT_EQ(Binding->getArray("regions")->size(), Prefixes.size());
      execute(Code, Harness);
      if (const char *Directory =
              std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
        const auto Base = std::string(Directory) + "/rtm-prefix-pair-" +
                          (LLVM ? "llvm-" : "high-") + Dispatch;
        std::ofstream(Base + ".c") << Code;
        std::ofstream(Base + ".json") << Report;
        std::ofstream(Base + ".test.c") << Code << Harness;
      }
    }
  RecordProperty("prefix_pairs", Prefixes.size());
}

TEST_F(X64ContinuationExportTest, RtmRegionsRejectForbiddenControlPrefixes) {
  unsigned Refusals = 0;
  for (const auto &[Prefix, Width] : x86TransactionTestPrefixes(true)) {
    if (std::none_of(Prefix.begin(), Prefix.end(), [](uint8_t Byte) {
          return Byte == 0x66 || Byte == 0xf2 || Byte == 0xf3;
        }))
      continue;
    for (uint8_t Control : {0xd5, 0xd6}) {
      auto Bytes = Prefix;
      Bytes.insert(Bytes.end(), {0x0f, 0x01, Control, 0xc3});
      load(Bytes);
      for (bool LLVM : {false, true}) {
        const auto Request =
            request(R"("instructions":[4096],"native_rtm_regions":true)");
        const char *Report = nullptr;
        EXPECT_EQ(neverd_x64_continuation_source_v1(
                      Session, Request.data(), Request.size(), LLVM, &Report),
                  nullptr);
        EXPECT_EQ(Report, nullptr);
        EXPECT_NE(take(neverd_last_error(Session)).find("0x1000"),
                  std::string::npos);
        ++Refusals;
      }
    }
  }
  RecordProperty("forbidden_control_exports", Refusals);
}
