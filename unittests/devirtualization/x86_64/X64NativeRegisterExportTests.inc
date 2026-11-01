//===- X64NativeRegisterExportTests.inc - Native register options -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Public bindings opt into exactly their native register bank.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest, NativeRegisterBindingsRemainIndependent) {
  load(
      {0x0f, 0x21, 0xc0, 0x0f, 0x23, 0xf8, 0x0f, 0x20, 0xe0, 0x0f, 0x22, 0xe0});
  for (bool Debug : {false, true})
    for (bool PageGlobal : {false, true})
      for (bool LLVM : {false, true}) {
        const auto Request = request(
            std::string(
                R"("instructions":[4096,4099,4102,4105],"native_debug_register_access":)") +
            (Debug ? "true" : "false") + ",\"native_page_global_access\":" +
            (PageGlobal ? "true" : "false"));
        auto [Source, Manifest] = source(Request, LLVM);
        auto Parsed = llvm::json::parse(Manifest);
        ASSERT_TRUE(bool(Parsed));
        const auto *Object = Parsed->getAsObject();
        ASSERT_NE(Object, nullptr);
        EXPECT_EQ(Object->getInteger("native_requests"),
                  (Debug ? 0 : 2) + (PageGlobal ? 0 : 2));
        const auto *Units = Object->getArray("units");
        ASSERT_NE(Units, nullptr);
        ASSERT_EQ(Units->size(), 4u);
        for (unsigned I = 0; I != 4; ++I) {
          const auto *Unit = (*Units)[I].getAsObject();
          ASSERT_NE(Unit, nullptr);
          EXPECT_EQ(Unit->getBoolean("native_boundary"),
                    !(I < 2 ? Debug : PageGlobal));
          if (I < 2 ? Debug : PageGlobal) {
            const auto *Effects = Unit->getArray("arbitrary_effects");
            ASSERT_NE(Effects, nullptr);
            EXPECT_EQ(Effects->size(), 6u);
          }
        }
        execute(Source, "int main(void) { return 0; }\n");
      }
}
