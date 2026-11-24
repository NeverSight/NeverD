//===- X64RtmFaultExportTests.inc - Observed XBEGIN fault source ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check an independently encoded #UD frame against both public C routes.
//===----------------------------------------------------------------------===//

llvm::json::Object xbeginFaultObservation() {
  llvm::json::Object Before{{"cpu", 2},
                            {"rip", 0x2000},
                            {"rsp", 0x900b},
                            {"cs", 0x18},
                            {"ss", 0x20},
                            {"eflags", 0x42},
                            {"rax", "0xc0de123456789abc"},
                            {"efer", 0x500},
                            {"cr4", 0x20},
                            {"cs_ar", 0xa09b}};
  auto After = Before;
  After["rip"] = 0x3700;
  After["rsp"] = 0x8fd8;
  std::string IDT(32 * 32, '0');
  IDT.replace(6 * 32, 32, "00371800008e00000000000000000000");
  return llvm::json::Object{{"before", std::move(Before)},
                            {"after", std::move(After)},
                            {"exception_idt", std::move(IDT)},
                            {"bytes", "C7F805000000"},
                            {"load_base", 0x2000},
                            {"frame",
                             "002000000000000018000000000000004200010000000000"
                             "0b900000000000002000000000000000"}};
}

std::string xbeginFaultFields(llvm::json::Object Capture) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "\"qemu_xbegin_fault_v1\":" << llvm::json::Value(std::move(Capture));
  return Text;
}

TEST_F(X64ContinuationExportTest,
       QemuXbeginFaultBindsOnlyTheObservedInstruction) {
  // Two XBEGINs and no XABORT. Only the first instruction was observed.
  load({0xc7, 0xf8, 5, 0, 0, 0, 0xc7, 0xf8, 5, 0, 0, 0, 0xc3});
  for (bool LLVM : {false, true})
    for (auto Dispatch : {"direct", "switch"}) {
      const auto Fields = xbeginFaultFields(xbeginFaultObservation());
      const auto [Source, Manifest] = source(
          request((llvm::Twine(R"("instructions":[4096,4102],"dispatch":")") +
                   Dispatch + "\"," + Fields)
                      .str()),
          LLVM);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed));
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      const auto *Capture = Report->getObject("qemu_xbegin_fault_v1");
      ASSERT_NE(Capture, nullptr);
      EXPECT_EQ(Report->getInteger("native_requests"), 1);
      EXPECT_EQ(Capture->getString("rip"), "0x2000");
      EXPECT_EQ(Capture->getString("image_pc"), "0x1000");
      EXPECT_EQ(Capture->getInteger("cpu"), 2);
      EXPECT_EQ(Capture->getInteger("vector"), 6);
      EXPECT_EQ(Capture->getString("bytes"), "C7F805000000");
      const auto *Steps = Report->getArray("units");
      ASSERT_NE(Steps, nullptr);
      ASSERT_EQ(Steps->size(), 2u);
      EXPECT_EQ((*Steps)[0].getAsObject()->getBoolean("native_boundary"),
                false);
      EXPECT_EQ((*Steps)[0].getAsObject()->getBoolean("rtm_body"), false);
      EXPECT_EQ((*Steps)[1].getAsObject()->getBoolean("native_boundary"), true);
      llvm::StringRef C(Source);
      EXPECT_FALSE(C.contains("cpuid") || C.contains("rdmsr") ||
                   C.contains("xbegin ") || C.contains("rtm_abort_pair") ||
                   C.contains_insensitive("c0de123456789abc"));
    }
}

TEST_F(X64ContinuationExportTest, QemuXbeginFaultRejectsContradictoryStops) {
  load({0xc7, 0xf8, 5, 0, 0, 0, 0xc3});
  const auto Refuse = [&](llvm::json::Object Capture,
                          llvm::StringRef Fields = R"("instructions":[4096])") {
    const std::string Query =
        request((Fields + "," + xbeginFaultFields(std::move(Capture))).str());
    const char *Receipt = nullptr;
    EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Query.data(),
                                                Query.size(), 0, &Receipt),
              nullptr);
    EXPECT_EQ(Receipt, nullptr);
  };
  for (const auto &[Key, Value] :
       std::initializer_list<std::pair<const char *, uint64_t>>{
           {"cpu", 3},
           {"rip", 0x2006},
           {"rip", 0x200b},
           {"rsp", 0x8fe0},
           {"cs", 0x10},
           {"ss", 0x28},
           {"rax", 0},
           {"eflags", 0x142},
           {"cr4", 0x1020},
           {"efer", 0x100},
           {"cs_ar", 0xc09b}}) {
    auto Capture = xbeginFaultObservation();
    (*Capture.getObject("after"))[Key] = Value;
    Refuse(std::move(Capture));
  }
  for (size_t I = 0; I < 5; ++I) {
    auto Capture = xbeginFaultObservation();
    auto Frame = Capture.getString("frame")->str();
    Frame[16 * I] = 'f';
    Capture["frame"] = std::move(Frame);
    Refuse(std::move(Capture));
  }
  for (llvm::StringRef Bytes :
       {"c7f804000000", "c7f8", "c7f8zz000000", "90f805000000"}) {
    auto Capture = xbeginFaultObservation();
    Capture["bytes"] = Bytes;
    Refuse(std::move(Capture));
  }
  auto Reserved = xbeginFaultObservation();
  (*Reserved.getObject("before"))["eflags"] = 0x4a;
  (*Reserved.getObject("after"))["eflags"] = 0x4a;
  auto ReservedFrame = Reserved.getString("frame")->str();
  ReservedFrame.replace(32, 2, "4a");
  Reserved["frame"] = std::move(ReservedFrame);
  Refuse(std::move(Reserved));
  auto Shared = xbeginFaultObservation();
  auto Gates = Shared.getString("exception_idt")->str();
  Gates.replace(1 * 32, 32, Gates.substr(6 * 32, 32));
  Shared["exception_idt"] = std::move(Gates);
  Refuse(std::move(Shared));
  auto WrongBase = xbeginFaultObservation();
  WrongBase["load_base"] = 0x3000;
  Refuse(std::move(WrongBase));
  Refuse(xbeginFaultObservation(), R"("instructions":[4102])");
  Refuse(xbeginFaultObservation(),
         R"("instructions":[4096],"native_rtm_abort_steps":"auto")");
  Refuse(xbeginFaultObservation(),
         R"("instructions":[4096],"paged_memory":true)");
}

TEST_F(X64ContinuationExportTest, QemuXbeginFaultFrameFoldsToTheObservedState) {
  if (!neverd::test::compilerReadsCurrentIR(NEVERD_TEST_CLANG))
    GTEST_SKIP() << "fixture compiler and project LLVM IR differ";
  load({0xc7, 0xf8, 5, 0, 0, 0, 0xc3});
  const auto Query = request(R"("instructions":[4096],)" +
                             xbeginFaultFields(xbeginFaultObservation()));
  const auto [Source, Manifest] = source(Query, false);
  // One scalar step takes exactly the same entry guards and memory operations
  // through either route. This also catches accidental general-backend work.
  EXPECT_EQ(source(Query, true).first, Source);
  const auto C = tmpFile("xbegin-fault.c"), IR = tmpFile("xbegin-fault.ll");
  const auto Folded = tmpFile("xbegin-fault-folded.ll");
  // Only guest memory addressing is mapped to a byte array. Compile twice to
  // inline the step and propagate its descriptor/stack stores through the CFG.
  // The input RAX stays symbolic. No generated instruction or program runs.
  std::ofstream(C) << R"C(
#include <stdint.h>
#include <string.h>
uint64_t neverd_step_v6_1000(uint8_t *) __attribute__((always_inline));
static uint8_t ram[0xc000];
static inline __attribute__((always_inline))
void read_ram(void *to, const void *addr, size_t n) {
    __builtin_memcpy(to, ram + (uintptr_t)addr, n);
}
static inline __attribute__((always_inline))
void write_ram(void *addr, const void *from, size_t n) {
    __builtin_memcpy(ram + (uintptr_t)addr, from, n);
}
#define __builtin_memcpy(d,s,n) _Generic((d), void *: write_ram, default: read_ram)((d),(s),(n))
)C" << Source << R"C(
#undef __builtin_memcpy
int proof_frame(uint64_t rax) {
    neverd_x64_state_v6 s = {0};
    s.PC=0x1000; s.GPR[0]=rax; s.GPR[4]=0x900b; s.RFlags=0x42;
    s.CS=(neverd_x64_segment){0,0xffffffff,0x18,0xa09b};
    s.SS=(neverd_x64_segment){0,0xffffffff,0x20,0xc093};
    s.GDTR.Limit=s.IDTR.Limit=0xffff;
    const uint64_t code=0x00af9b000000ffffULL, hi=0;
    // A different live handler from the recorded gate.
    const uint64_t gate=0x00008e0000184900ULL;
    __builtin_memcpy(ram+0x18,&code,8);
    __builtin_memcpy(ram+96,&gate,8);
    __builtin_memcpy(ram+104,&hi,8);
    __builtin_memcpy(ram+0xa000,&s,sizeof s);
    uint64_t status=neverd_step_v6_1000((uint8_t *)(uintptr_t)0xa000);
    uint64_t frame[5];
    __builtin_memcpy(&s,ram+0xa000,sizeof s);
    __builtin_memcpy(frame,ram+0x8fd8,sizeof frame);
    return status==7 && s.PC==0x4900 && s.GPR[0]==rax && s.GPR[4]==0x8fd8 &&
           s.RFlags==0x42 && frame[0]==0x1000 && frame[1]==0x18 &&
           frame[2]==0x10042 && frame[3]==0x900b && frame[4]==0x20;
}
)C";
  const auto Built =
      exec(NEVERD_TEST_CLANG, {"-std=c11", "-O3", "-ffreestanding", "-S",
                               "-emit-llvm", C.string(), "-o", IR.string()});
  ASSERT_TRUE(Built.ok()) << Built.err;
  const auto Optimized =
      exec(NEVERD_TEST_CLANG, {"-O2", "-S", "-emit-llvm", "-x", "ir",
                               IR.string(), "-o", Folded.string()});
  ASSERT_TRUE(Optimized.ok()) << Optimized.err;
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  const auto Module = llvm::parseIRFile(Folded.string(), Diagnostic, Context);
  ASSERT_NE(Module, nullptr);
  const auto *Proof = Module->getFunction("proof_frame");
  ASSERT_NE(Proof, nullptr);
  ASSERT_EQ(Proof->size(), 1u);
  const auto *Ret =
      llvm::dyn_cast<llvm::ReturnInst>(Proof->front().getTerminator());
  ASSERT_NE(Ret, nullptr);
  const auto *Result = llvm::dyn_cast<llvm::ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(Result, nullptr);
  EXPECT_TRUE(Result->isOne());
}
