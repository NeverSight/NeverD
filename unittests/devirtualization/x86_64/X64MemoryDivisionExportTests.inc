//===- X64MemoryDivisionExportTests.inc - Native memory division --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original memory DIV/IDIV against both source routes, including #DE frames.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest,
       MemoryDivisionMatchesOriginalAddressesAndErrors) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "native memory division qualification requires Linux x64 KVM";
#else
  std::vector<uint8_t> Code;
  std::string Entries, Cases = R"C(
static const struct {uint64_t pc;unsigned width,sign,form;unsigned char code[13];} cases[]={
)C";
  for (unsigned Width : {1u, 2u, 4u, 8u})
    for (bool Signed : {false, true})
      for (unsigned Form = 0; Form != 10; ++Form) {
        const uint64_t PC = 0x1000 + Code.size();
        std::vector<uint8_t> Bytes;
        if (Form >= 8)
          Bytes.push_back(Form == 8 ? 0x64 : 0x65);
        if (Width == 2)
          Bytes.push_back(0x66);
        if (Form == 5)
          Bytes.push_back(0x67);
        if (Width == 8 || Form == 3)
          Bytes.push_back(0x40 | (Width == 8 ? 8 : 0) | (Form == 3 ? 3 : 0));
        Bytes.push_back(Width == 1 ? 0xf6 : 0xf7);
        const uint8_t Operation = Signed ? 0x38 : 0x30;
        switch (Form) {
        case 0:
          Bytes.push_back(Operation | 1);
          break; // [RCX].
        case 1:
          Bytes.insert(Bytes.end(), {uint8_t(Operation | 0x45), 0xf9});
          break;
        case 2:
          Bytes.insert(Bytes.end(), {uint8_t(Operation | 0x44), 0x8c, 0xf9});
          break;
        case 3:
          Bytes.insert(Bytes.end(), {uint8_t(Operation | 0x44), 0xe5, 0xf9});
          break;
        case 4: {
          Bytes.push_back(Operation | 5);
          const uint32_t Displacement = 0x9103 - (PC + Bytes.size() + 4);
          for (unsigned I = 0; I != 4; ++I)
            Bytes.push_back(Displacement >> (8 * I));
          break;
        }
        case 5:
          Bytes.insert(Bytes.end(), {uint8_t(Operation | 0x44), 0x4b, 0xf9});
          break;
        case 6:
          Bytes.push_back(Operation);
          break; // Divisor address aliases AX.
        case 7:
          Bytes.push_back(Operation | 2);
          break; // Address aliases DX.
        default:
          Bytes.insert(Bytes.end(), {uint8_t(Operation | 0x41), 0xf9});
          break;
        }
        if (!Entries.empty())
          Entries += ',';
        Entries += std::to_string(PC);
        Cases += "{" + std::to_string(PC) + "ULL," + std::to_string(Width) +
                 "," + std::to_string(Signed) + "," + std::to_string(Form) +
                 ",{" + std::to_string(Bytes.size());
        for (auto Byte : Bytes)
          Cases += "," + std::to_string(Byte);
        Cases += "}},\n";
        Code.insert(Code.end(), Bytes.begin(), Bytes.end());
      }
  Cases += "};\n";
  load(Code);
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    auto [Source, Manifest] = source(
        request(R"("event_delivery":true,"instructions":[)" + Entries + "]"),
        LLVM);
    const auto File =
        tmpFile(LLVM ? "memory-division-llvm.c" : "memory-division-high.c");
    std::ofstream(File) << Source << Seed << Cases << R"C(
#undef CHECK
#define CHECK(c) do {if(!(c)) { __asm__ volatile("hlt"::"a"(__LINE__),"b"(case_index):"memory"); __builtin_unreachable(); }} while(0)
int main(void) {
  unsigned char before[8192],after[8192];unsigned comparisons=0;
  const unsigned flag_bits[]={11,7,6,4,2,0};
  for(unsigned case_index=0;case_index<sizeof(cases)/sizeof(*cases);++case_index) {
    const unsigned width=cases[case_index].width,n=width*8,form=cases[case_index].form;
    const uint64_t mask=width==8?UINT64_MAX:(1ULL<<n)-1,sign=1ULL<<(n-1);
    const uint64_t lows[]={0,1,mask,sign,sign-1},highs[]={0,1,mask,sign};
    const uint64_t divisors[]={0,1,3,mask,sign};
    for(unsigned a=0;a<5;++a)for(unsigned d=0;d<4;++d)for(unsigned c=0;c<5;++c) {
      neverd_x64_state_v6 s=seed();s.PC=cases[case_index].pc;s.NMIBlocked=0;
      s.FSBase=form==8?0x500:0;s.GSBase=form==9?0x500:0;
      s.GDTR=(neverd_x64_descriptor_table){0x8000,63,0,0};
      s.IDTR=(neverd_x64_descriptor_table){0x8200,0x7ff,0,0};
      s.TR=(neverd_x64_segment){0x8a00,103,48,0x8b};s.GPR[4]=0x8f27;
      s.GPR[0]=(s.GPR[0]&~mask)|lows[a];s.GPR[2]=(s.GPR[2]&~mask)|highs[d];
      if(width==1)s.GPR[0]=(s.GPR[0]&~0xff00ULL)|(highs[d]<<8);
      s.GPR[1]=0x9103;
      if(form==1)s.GPR[5]=0x910a;
      if(form==2){s.GPR[1]=5;s.GPR[4]=0x910a-20;}
      if(form==3){s.GPR[12]=3;s.GPR[13]=0x910a-24;}
      if(form==5){s.GPR[1]=0x400000005ULL;s.GPR[3]=0xbeef00009100ULL;}
      if(form==6)s.GPR[0]=0x9103;
      if(form==7)s.GPR[2]=0x9103;
      if(form>=8)s.GPR[1]=0x8c0a;
      memset((void *)0x8000,0xa5,8192);
      uint64_t descriptor=0x00af9a000000ffffULL,gate=0x1800|(24ULL<<16)|(0x8eULL<<40),zero=0;
      memcpy((void *)0x8018,&descriptor,8);memcpy((void *)0x8200,&gate,8);memcpy((void *)0x8208,&zero,8);
      memcpy((void *)0x9103,&divisors[c],width);
      neverd_x64_state_v6 want=s;memcpy(before,(void *)0x8000,8192);
      __asm__ volatile("outb %%al,$0xe9"::"a"(21),"D"(&want),"S"(cases[case_index].code):"memory");
      memcpy(after,(void *)0x8000,8192);memcpy((void *)0x8000,before,8192);
      const int fault=want.PC==0x1800;
      if(!fault)for(unsigned f=0;f<6;++f)s.Undefined[f]=want.Undefined[f]=(want.RFlags>>flag_bits[f])&1;
      uint64_t budget=1;
      CHECK(neverd_x64_continue_v6(&s,&budget)==(fault?7:9));
      CHECK(!budget&&!memcmp(&s,&want,sizeof(s))&&!memcmp((void *)0x8000,after,8192));
      ++comparisons;
    }
  }
  return comparisons==8000?0:1;
}
)C";
    for (const char *Optimization : {"O0", "O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Ran =
          exec("python3",
               {std::string(TEST_SOURCE_DIR) + "/run_x64_continuation_kvm.py",
                "--clang", NEVERD_TEST_CLANG, "--source", File.string(),
                "--optimization", Optimization, "--compiler-runtime"});
      if (Ran.exitCode == 77)
        GTEST_SKIP() << Ran.err;
      ASSERT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode;
    }
    if (const char *Directory =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Directory);
      const std::string Prefix =
          LLVM ? "memory-division-llvm" : "memory-division-high";
      std::ofstream(fs::path(Directory) / (Prefix + ".json")) << Manifest;
      std::ofstream(fs::path(Directory) / (Prefix + ".source.c")) << Source;
      for (const auto &Entry : fs::directory_iterator(tmp()))
        if (Entry.is_regular_file() &&
            Entry.path().filename().string().starts_with(Prefix))
          fs::copy_file(Entry.path(),
                        fs::path(Directory) / Entry.path().filename(),
                        fs::copy_options::overwrite_existing);
    }
  }
#endif
}
