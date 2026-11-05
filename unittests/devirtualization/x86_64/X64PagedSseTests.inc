//===- X64PagedSseTests.inc - Static vector source proofs -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile actual exported SSE bodies against an independent operand oracle.
/// Inspect optimizer proofs; never execute the bodies or an instruction CPU.
//===----------------------------------------------------------------------===//

class X64PagedSseTest : public X64ContinuationExportTest {
protected:
  struct Case {
    std::vector<uint8_t> Bytes;
    const char *Address;
    unsigned Destination, Source;
    bool Store = false, Stack = false, Xor = false;
  };
  const std::vector<Case> Cases = {
      {{0x0f, 0x57, 0xc0}, nullptr, 0, 0, false, false, true},
      {{0x45, 0x0f, 0x57, 0xce}, nullptr, 9, 14, false, false, true},
      {{0x0f, 0x10, 0xc1}, nullptr, 0, 1},
      {{0x0f, 0x10, 0x00}, "s.GPR[0]", 0, 0},
      {{0x0f, 0x11, 0x08}, "s.GPR[0]", 0, 1, true},
      {{0xf3, 0x45, 0x0f, 0x6f, 0x55, 7}, "s.GPR[13]+7", 10, 0},
      {{0xf3, 0x0f, 0x7f, 0x54, 0x24, 3}, "s.GPR[4]+3", 0, 2, true, true},
      {{0x0f, 0x10, 0x1d, 32, 0, 0, 0}, "0x1097", 3, 0},
      {{0x67, 0x0f, 0x10, 0x65, 7}, "(uint32_t)(s.GPR[5]+7)", 4, 0},
      {{0xf3, 0x0f, 0x6f, 0xe5}, nullptr, 4, 5},
      {{0x41, 0x0f, 0x10, 0x06}, "s.GPR[14]", 0, 0},
  };

  std::pair<std::string, std::string> exportCases(bool LLVM) {
    std::vector<uint8_t> Bytes;
    std::string Entries;
    for (const auto &C : Cases) {
      if (!Entries.empty())
        Entries += ',';
      Entries += std::to_string(0x1000 + Bytes.size());
      Bytes.insert(Bytes.end(), C.Bytes.begin(), C.Bytes.end());
      Bytes.resize((Bytes.size() + 15) & ~size_t(15), 0x90);
    }
    load(Bytes);
    const auto Request =
        request("\"paged_memory\":true,\"instructions\":[" + Entries + "]");
    const char *Report = nullptr;
    auto Source = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), LLVM, &Report));
    EXPECT_FALSE(Source.empty()) << take(neverd_last_error(Session));
    return {std::move(Source), take(Report)};
  }
};

TEST_F(X64PagedSseTest, AdmissionAndBothSourceRoutes) {
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto [Source, Report] = exportCases(LLVM);
    auto Manifest = llvm::json::parse(Report);
    ASSERT_TRUE(bool(Manifest));
    EXPECT_EQ(Manifest->getAsObject()->getInteger("modeled"), Cases.size());
    EXPECT_EQ(Manifest->getAsObject()->getInteger("native_requests"), 0);
    const auto File = tmpFile("paged-sse.c");
    std::ofstream(File) << Source;
    for (const char *Level : {"-O0", "-O2"}) {
      const auto Built =
          exec(NEVERD_TEST_CLANG, {"-std=c11", Level, "-Werror=uninitialized",
                                   "-Werror=return-type", "-c", File.string(),
                                   "-o", tmpFile("paged-sse.o").string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
    }
  }
}

TEST_F(X64PagedSseTest, OtherVectorFaultDomainsRemainNative) {
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0x0f, 0x28, 0x00}, // MOVAPS alignment.
           {0x0f, 0x57, 0x00}, // XORPS memory alignment.
           {0x0f, 0x58, 0xc1}, // ADDPS FP exceptions.
           {0x66, 0x0f, 0xef,
            0xc0}, // Other bitvectors stay outside this model.
           {0xc5, 0xf8, 0x10, 0x00}, // VEX state/availability.
           {0xf3, 0x0f, 0x10, 0xc1}, // MOVSS partial-register semantics.
           {0x64, 0x0f, 0x10,
            0x18}, // Unaligned FS/GS forms lack the lift audit.
           {0x65, 0x0f, 0x11, 0x18},
       }) {
    load(Bytes);
    for (bool LLVM : {false, true}) {
      const auto Request =
          request(R"("paged_memory":true,"instructions":[4096])");
      const char *Report = nullptr;
      const auto Source = take(neverd_x64_continuation_source_v1(
          Session, Request.data(), Request.size(), LLVM, &Report));
      ASSERT_FALSE(Source.empty()) << take(neverd_last_error(Session));
      auto Manifest = llvm::json::parse(take(Report));
      ASSERT_TRUE(bool(Manifest));
      EXPECT_EQ(Manifest->getAsObject()->getInteger("native_requests"), 1);
    }
  }
}

TEST_F(X64PagedSseTest, ProvesValuesAccessExtentAndFaultStops) {
  const char Oracle[] = R"C(
typedef struct {
    uint64_t lo, hi;
    void *transaction;
    uint64_t address, flags, stored_lo, stored_hi;
    unsigned calls, bad, memory, fail;
} sse_exchange;
uint64_t neverd_x64_paged_access_v1(void *opaque, uint64_t address,
                                  uint64_t size, uint64_t flags) {
    sse_exchange *e = opaque;
    ++e->calls;
    e->bad |= e->transaction != e;
    if (e->calls == 1) {
        e->bad |= flags != 20 || size != 0 || address != e->memory;
        return e->fail == 1;
    }
    e->bad |= e->calls != 2 || size != 16 || address != e->address
              || flags != e->flags;
    if (e->fail == 2) return 1;
    if (flags & 1) {
        e->stored_lo = e->lo; e->stored_hi = e->hi;
    } else {
        e->lo = 0xfedcba9876543210ULL; e->hi = 0x0123456789abcdefULL;
    }
    return 0;
}
static neverd_x64_state_v6 sse_seed(uint64_t pc, uint64_t a, uint64_t b) {
    neverd_x64_state_v6 s = {0};
    s.PC = pc; s.RFlags = 0x246; s.MXCSR = 0x1f80;
    s.CS.Selector = 8; s.CS.Attributes = 0xa09b;
    s.SS.Selector = 16; s.SS.Attributes = 0xc093;
    s.FSBase = 0x30000000; s.GSBase = 0x50000000;
    for (unsigned i = 0; i < 16; ++i) s.GPR[i] = 0x123400002000ULL + i*256;
    #pragma clang loop unroll(full)
    for (unsigned i = 0; i < 32; ++i)
        #pragma clang loop unroll(full)
        for (unsigned j = 0; j < 64; ++j) s.ZMM[i][j] = i*13+j*7;
    memcpy(&s.ZMM[9][0], &a, 8); memcpy(&s.ZMM[9][8], &b, 8);
    memcpy(&s.ZMM[14][0], &b, 8); memcpy(&s.ZMM[14][8], &a, 8);
    return s;
}
static uint64_t sse_difference(const neverd_x64_state_v6 *a,
                               const neverd_x64_state_v6 *b) {
    uint64_t different = 0;
    #pragma clang loop unroll(full)
    for (unsigned i = 0; i < sizeof(*a); i += 8) {
        uint64_t x, y;
        memcpy(&x, (const uint8_t *)a+i, 8);
        memcpy(&y, (const uint8_t *)b+i, 8);
        different |= x ^ y;
    }
    return different;
}
)C";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto [Source, Report] = exportCases(LLVM);
    std::string Probes = Oracle;
    unsigned Count = 0;
    for (unsigned I = 0; I != Cases.size(); ++I) {
      const auto &C = Cases[I];
      const auto PC = 0x1000 + 16 * I;
      for (unsigned Fail = 0; Fail != (C.Address ? 3u : 2u); ++Fail) {
        ++Count;
        Probes += "int proof_" + std::to_string(I) + "_" +
                  std::to_string(Fail) + "(uint64_t a, uint64_t b) {\n";
        Probes += "neverd_x64_state_v6 s=sse_seed(" + std::to_string(PC) +
                  ",a,b), expected=s;\n";
        Probes += "sse_exchange e={0}; e.transaction=&e; e.memory=" +
                  std::to_string(bool(C.Address)) +
                  "; e.fail=" + std::to_string(Fail) + ";\n";
        if (C.Address)
          Probes += "e.address=" + std::string(C.Address) + "; e.flags=" +
                    std::to_string(unsigned(C.Store) | (C.Stack ? 8 : 0)) +
                    ";\n";
        Probes += "uint64_t low=0, high=0;\n";
        if (!Fail) {
          Probes +=
              "expected.PC=" + std::to_string(PC + C.Bytes.size()) + ";\n";
          const auto Src = "s.ZMM[" + std::to_string(C.Source) + "]";
          const auto Dst =
              "expected.ZMM[" + std::to_string(C.Destination) + "]";
          if (C.Store)
            Probes +=
                "memcpy(&low," + Src + ",8); memcpy(&high," + Src + "+8,8);\n";
          else if (C.Address)
            Probes += "low=0xfedcba9876543210ULL; high=0x0123456789abcdefULL;"
                      "memcpy(" +
                      Dst + ",&low,8); memcpy(" + Dst + "+8,&high,8);\n";
          else if (C.Xor)
            Probes += "{ __uint128_t x,y; memcpy(&x," + Dst +
                      ",16); memcpy(&y," + Src + ",16); x^=y; memcpy(" + Dst +
                      ",&x,16); }\n";
          else
            Probes += "memcpy(" + Dst + "," + Src + ",16);\n";
        }
        Probes += "uint64_t result=neverd_paged_neverd_step_v6_" +
                  llvm::utohexstr(PC) + "((uint8_t *)&s,(uint8_t *)&e);\n";
        Probes += "return result!=" + std::to_string(Fail ? 17 : 0) +
                  " || e.bad || e.calls!=" +
                  std::to_string(Fail == 1 || !C.Address ? 1 : 2) +
                  " || sse_difference(&s,&expected)";
        if (C.Store && !Fail)
          Probes += " || e.stored_lo!=low || e.stored_hi!=high";
        else
          Probes += " || e.stored_lo || e.stored_hi";
        Probes += ";}\n";
      }
    }
    const auto File = tmpFile("paged-sse-proof.c");
    const auto IR = tmpFile("paged-sse-proof.ll");
    std::ofstream(File) << Source << Probes;
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", "-O2", "-mllvm", "-inline-threshold=10000", "-mllvm",
              "-inline-cold-callsite-threshold=10000", "-S", "-emit-llvm",
              File.string(), "-o", IR.string()});
    if (const char *Root = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      const fs::path Directory(Root);
      fs::create_directories(Directory);
      const auto Name = LLVM ? "paged-sse-llvm" : "paged-sse-high";
      fs::copy_file(File, Directory / (std::string(Name) + ".c"),
                    fs::copy_options::overwrite_existing);
      if (fs::exists(IR))
        fs::copy_file(IR, Directory / (std::string(Name) + ".ll"),
                      fs::copy_options::overwrite_existing);
    }
    ASSERT_TRUE(Built.ok()) << Built.err;
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    const auto Module = llvm::parseIRFile(IR.string(), Diagnostic, Context);
    ASSERT_NE(Module, nullptr);
    unsigned Proven = 0;
    for (const auto &Function : *Module)
      if (Function.getName().starts_with("proof_")) {
        SCOPED_TRACE(Function.getName().str());
        ++Proven;
        ASSERT_EQ(Function.size(), 1u);
        const auto *Return =
            llvm::dyn_cast<llvm::ReturnInst>(Function.front().getTerminator());
        ASSERT_NE(Return, nullptr);
        const auto *Value =
            llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
        ASSERT_NE(Value, nullptr);
        EXPECT_TRUE(Value->isZero());
      }
    EXPECT_EQ(Proven, Count);
  }
}
