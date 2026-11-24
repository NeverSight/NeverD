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
