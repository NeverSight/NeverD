//===- X64DivisionContinuationTests.inc - Native division comparison ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included inside the continuation source fixture namespace.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationSourceTest, DivisionWidthsAndAliasesMatchNativeFaults) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "the independent DIV/IDIV oracle requires Linux x64";
#else
  std::vector<uint8_t> Bytes;
  std::vector<va_t> Entries;
  std::string Dispatch =
      "static uint64_t step(unsigned i,uint8_t *s) { switch(i) {\n";
  std::string Cases = "static const struct {unsigned "
                      "width,sign,reg,shift,size;uint64_t pc;} cases[]={\n";
  for (bool Signed : {false, true})
    for (unsigned Width : {1u, 2u, 4u, 8u})
      for (unsigned Register : {0u, 1u, 2u, 4u, 7u, 8u, 15u})
        for (bool HighByte : {false, true}) {
          if (HighByte && (Width != 1 || (Register != 4 && Register != 7)))
            continue;
          const va_t PC = 0x1000 + Bytes.size();
          const auto Before = Bytes.size();
          if (Width == 2)
            Bytes.push_back(0x66);
          if (Width == 8 || Register >= 8 || (Width == 1 && !HighByte))
            Bytes.push_back(0x40 | (Width == 8 ? 8 : 0) | (Register >= 8));
          Bytes.push_back(Width == 1 ? 0xf6 : 0xf7);
          Bytes.push_back((Signed ? 0xf8 : 0xf0) | (Register & 7));
          Dispatch += "case " + std::to_string(Entries.size()) +
                      ":return neverd_step_v2_" + llvm::utohexstr(PC) +
                      "(s);\n";
          Entries.push_back(PC);
          Cases += "{" + std::to_string(Width) + "," + std::to_string(Signed) +
                   "," + std::to_string(HighByte ? Register - 4 : Register) +
                   "," + std::to_string(HighByte ? 8 : 0) + "," +
                   std::to_string(Bytes.size() - Before) + "," +
                   std::to_string(PC) + "ULL},\n";
        }
  Dispatch += "default:abort();}}\n";
  Cases += "};\n";
  const std::string Harness =
      "\n#include <stdio.h>\n#include \"" + std::string(TEST_SOURCE_DIR) +
      "/fixtures/x86_division_oracle.h\"\n" + Dispatch + Cases + R"C(
int main(void) {
  division_install();unsigned comparisons=0;
  const unsigned bits[]={11,7,6,4,2,0};
  for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
    const unsigned width=cases[i].width,n=width*8,reg=cases[i].reg,shift=cases[i].shift;
    const uint64_t mask=width==8?UINT64_MAX:(1ULL<<n)-1;
    const uint64_t sign=1ULL<<(n-1);
    const uint64_t lows[]={0,1,mask,sign,sign-1};
    const uint64_t highs[]={0,1,mask,sign};
    const uint64_t divisors[]={0,1,3,mask,sign};
    for(unsigned a=0;a<5;++a)for(unsigned d=0;d<4;++d)
      for(unsigned c=0;c<5;++c)for(unsigned pattern=0;pattern<2;++pattern) {
        state_v2_t s=seed_v2(cases[i].pc);
        s.core.gpr[0]=(s.core.gpr[0]&~mask)|lows[a];
        s.core.gpr[2]=(s.core.gpr[2]&~mask)|highs[d];
        if(width==1)s.core.gpr[0]=(s.core.gpr[0]&~0xff00ULL)|(highs[d]<<8);
        s.core.gpr[reg]=(s.core.gpr[reg]&~(mask<<shift))|(divisors[c]<<shift);
        for(unsigned f=0;f<6;++f)s.core.arbitrary[f]=pattern;
        state_v2_t expected=s;
        uint64_t out_a=0,out_d=0;
        const int fault=division_native(width,cases[i].sign,s.core.gpr[0],s.core.gpr[2],
                                       s.core.gpr[reg]>>shift,&out_a,&out_d);
        const uint64_t status=step(i,ARG(&s));
        CHECK(status==(fault?5:0));
        if(!fault) {
          expected.core.gpr[0]=out_a;expected.core.gpr[2]=out_d;
          expected.core.pc+=cases[i].size;expected.core.flags&=~0x8d5ULL;
          for(unsigned f=0;f<6;++f)expected.core.flags|=(uint64_t)pattern<<bits[f];
        }
        CHECK(!memcmp(&s,&expected,sizeof(s)));++comparisons;
      }
  }
  printf("%u native DIV/IDIV state comparisons\n",comparisons);
  return 0;
}
)C";
  run(std::move(Bytes), std::move(Entries), Harness, false, controlsOptions(),
      {}, false, "division");
#endif
}
