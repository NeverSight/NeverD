//===- SessionRecoverySourceABITests.inc - Recovery storage contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check size-delimited source ABI selection and preservation of older APIs.
//===----------------------------------------------------------------------===//

TEST_F(SessionCAPITest, InterpreterRecoveryV10ValidatesSourceLayouts) {
  const auto Input = write("source-layout.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v10, machine_source_abi),
            sizeof(neverd_devirtualize_options_v9));
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v10, reserved),
            sizeof(neverd_devirtualize_options_v9) + sizeof(uint32_t));
  for (bool Machine : {false, true}) {
    auto Recover = Machine ? neverd_devirtualize_machine_source_v10
                           : neverd_devirtualize_source_v10;
    const char *Report = "previous";
    EXPECT_EQ(Recover(nullptr, Entry, nullptr, &Report), nullptr);
    EXPECT_EQ(Report, nullptr);
    EXPECT_FALSE(takeString(Recover(Session, Entry, nullptr, &Report)).empty());
    auto DefaultReport = llvm::json::parse(takeString(Report));
    ASSERT_TRUE(bool(DefaultReport));
    if (Machine)
      EXPECT_EQ(DefaultReport->getAsObject()->getInteger("sourceStateBytes"),
                136);
    size_t SizeOnly = sizeof(size_t);
    neverd_devirtualize_options_v10 Short{};
    Short.base.base.base.base.base.base.base.base.base.struct_size =
        sizeof(Short) - 1;
    for (const auto *O :
         {reinterpret_cast<const neverd_devirtualize_options_v10 *>(&SizeOnly),
          static_cast<const neverd_devirtualize_options_v10 *>(&Short)}) {
      EXPECT_EQ(Recover(Session, Entry, O, &Report), nullptr);
      EXPECT_NE(takeString(Report).find("complete v10 structure"),
                std::string::npos);
    }
    for (unsigned Kind = 0; Kind != 3; ++Kind)
      for (bool LLVM : {false, true}) {
        struct Future {
          neverd_devirtualize_options_v10 Options;
          uint64_t Opaque;
        } F{};
        F.Opaque = UINT64_MAX;
        auto &O = F.Options;
        O.base.base.base.base.base.base.base.base.base.struct_size = sizeof(F);
        O.base.base.base.base.base.base.base.base.base.use_llvm = LLVM;
        O.machine_source_abi = Kind;
        const auto Source = takeString(Recover(Session, Entry, &O, &Report));
        auto Parsed = llvm::json::parse(takeString(Report));
        ASSERT_TRUE(bool(Parsed));
        const bool Allowed = Machine || Kind == 0;
        EXPECT_EQ(!Source.empty(), Allowed)
            << takeString(neverd_last_error(Session));
        if (Machine && Allowed) {
          EXPECT_EQ(Parsed->getAsObject()->getInteger("sourceStateBytes"),
                    Kind == 0   ? 136
                    : Kind == 1 ? 160
                                : 416);
          EXPECT_EQ(Parsed->getAsObject()->getString("sourceABI"),
                    Kind == 0   ? "x64-machine-state-v1"
                    : Kind == 1 ? "x64-machine-query-state-v1"
                                : "x64-machine-query-state-v2");
        }
      }
    for (unsigned Bad = 0; Bad != 4; ++Bad) {
      neverd_devirtualize_options_v10 O{};
      O.base.base.base.base.base.base.base.base.base.struct_size = sizeof(O);
      if (Bad == 0)
        O.machine_source_abi = 3;
      else if (Bad == 1)
        O.machine_source_abi = UINT32_MAX;
      else if (Bad == 2)
        O.reserved = 1;
      else
        O.base.reserved = 1;
      EXPECT_EQ(Recover(Session, Entry, &O, nullptr), nullptr);
    }
  }
  neverd_devirtualize_options_v10 Future{};
  Future.base.base.base.base.base.base.base.base.base.struct_size =
      sizeof(Future);
  Future.machine_source_abi = Future.reserved = UINT32_MAX;
  for (auto Recover :
       {neverd_devirtualize_source_v9, neverd_devirtualize_machine_source_v9})
    EXPECT_FALSE(
        takeString(Recover(Session, Entry, &Future.base, nullptr)).empty());

  const uint8_t Code[]{0xb8, 7, 0, 0, 0, 0xb9, 3, 0, 0, 0, 0x0f, 0xa2, 0xc3};
  const auto Query =
      write("query-layout.elf",
            makeNativeELF(false, 0x400000, {},
                          std::string_view(reinterpret_cast<const char *>(Code),
                                           sizeof(Code))));
  ASSERT_EQ(neverd_session_load(Session, Query.c_str()), 1);
  for (uint32_t Kind : {NEVERD_DEVIRTUALIZE_MACHINE_QUERIES_V1,
                        NEVERD_DEVIRTUALIZE_MACHINE_QUERIES_XMM_V2}) {
    neverd_devirtualize_options_v10 O{};
    O.base.base.base.base.base.base.base.base.base.struct_size = sizeof(O);
    O.machine_source_abi = Kind;
    EXPECT_FALSE(takeString(neverd_devirtualize_machine_source_v10(
                                Session, neverd_session_entry_addr(Session), &O,
                                nullptr))
                     .empty());
    EXPECT_EQ(
        neverd_devirtualize_machine_source_v9(
            Session, neverd_session_entry_addr(Session), &O.base, nullptr),
        nullptr);
  }
}
