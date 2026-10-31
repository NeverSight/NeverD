//===- X64RepeatedTransferContinuationTests.inc - Live REP state ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included after the shared continuation fixture. Ordered element oracles
/// qualify runtime counts, direction, overlap and complete state preservation.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationSourceTest, RepeatedTransfersPreserveRuntimeState) {
  for (bool Controls : {false, true}) {
    for (bool Fill : {false, true}) {
      for (unsigned Width : {1u, 2u, 4u, 8u}) {
        SCOPED_TRACE(Controls);
        SCOPED_TRACE(Fill);
        SCOPED_TRACE(Width);
        std::vector<uint8_t> Bytes{0xf3};
        if (Width == 2 || Width == 8)
          Bytes.push_back(Width == 2 ? 0x66 : 0x48);
        Bytes.push_back(Fill ? (Width == 1 ? 0xaa : 0xab)
                             : (Width == 1 ? 0xa4 : 0xa5));
        std::string Harness;
        llvm::raw_string_ostream OS(Harness);
        OS << "\n#define WIDTH " << Width << "\n#define FILL " << Fill
           << "\n#define LENGTH " << Bytes.size() << '\n';
        if (Controls)
          OS << "#define BOX state_v2_t\n#define SEED seed_v2\n"
                "#define CORE(s) (&(s).core)\n"
                "#define STEP neverd_step_v2_1000\n";
        else
          OS << "#define BOX state_t\n#define SEED seed\n"
                "#define CORE(s) (&(s))\n#define STEP neverd_step_1000\n";
        OS << R"C(
int main(void) {
  const unsigned counts[]={0,1,2,3,9,33};
  const int gaps[]={-32,-2,-1,0,1,2,32};
  for(unsigned trial=0;trial<2;++trial)for(unsigned backward=0;backward<2;++backward)
    for(unsigned ci=0;ci<sizeof(counts)/sizeof(counts[0]);++ci)
      for(unsigned gi=0;gi<sizeof(gaps)/sizeof(gaps[0]);++gi) {
        unsigned char actual[768],expected[sizeof(actual)];
        for(unsigned i=0;i<sizeof(actual);++i)actual[i]=(i*29+trial*77)&255;
        memcpy(expected,actual,sizeof(actual));
        BOX box=SEED(0x1000);
        state_t *s=CORE(box);
        const unsigned count=counts[ci];
        const int src=backward?512:128,dst=src+gaps[gi];
        const int delta=backward?-WIDTH:WIDTH;
        s->gpr[0]=trial?0xfedcba9876543210ULL:0x0123456789abcdefULL;
        s->gpr[1]=count;
        s->gpr[6]=count?(uintptr_t)(actual+src):UINT64_MAX;
        s->gpr[7]=count?(uintptr_t)(actual+dst):0;
        s->flags=0x202|(trial?0x8d5:0)|(backward<<10);
        for(unsigned i=0;i<8;++i)s->arbitrary[i]=0xbad0cafe12340000ULL+i;
        BOX want=box;
        state_t *w=CORE(want);
        for(unsigned i=0;i<count;++i) {
          unsigned char element[WIDTH];
          if(FILL)memcpy(element,&s->gpr[0],WIDTH);
          else memcpy(element,expected+src+(int)i*delta,WIDTH);
          memcpy(expected+dst+(int)i*delta,element,WIDTH);
        }
        w->gpr[1]=0;
        if(!FILL)w->gpr[6]+=(uint64_t)((int)count*delta);
        w->gpr[7]+=(uint64_t)((int)count*delta);
        w->pc+=LENGTH;
        CHECK(STEP(ARG(&box))==0);
        CHECK(!memcmp(&box,&want,sizeof(box)));
        CHECK(!memcmp(actual,expected,sizeof(actual)));
      }
  return 0;
}
)C";
        run(Bytes, {0x1000}, Harness, false,
            Controls ? controlsOptions() : options());
      }
    }
  }
}

TEST_F(X64ContinuationSourceTest, RepeatedTransfersKeepUnownedFormsExplicit) {
  for (const auto &Bytes :
       {std::vector<uint8_t>{0x64, 0xf3, 0xa4}, // Segmented MOVS.
        std::vector<uint8_t>{0x65, 0xf3, 0xa5},
        std::vector<uint8_t>{0x67, 0xf3, 0xa4}, // Address-size override.
        std::vector<uint8_t>{0x67, 0xf3, 0xab},
        std::vector<uint8_t>{0xf3, 0xa6}, // CMPS, SCAS, LODS, INS, OUTS.
        std::vector<uint8_t>{0xf3, 0xae}, std::vector<uint8_t>{0xf3, 0xac},
        std::vector<uint8_t>{0xf3, 0x6c}, std::vector<uint8_t>{0xf3, 0x6e}}) {
    for (auto O : {options(), controlsOptions()}) {
      auto Units = buildX64ContinuationSource(image(Bytes), {0x1000}, O);
      ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
      ASSERT_EQ(Units->size(), 1u);
      EXPECT_TRUE(Units->front().RequiresNativeExecution);
      EXPECT_FALSE(Units->front().Diagnostic.empty());
    }
  }
}
