//===- X86PortableSystemSourceTests.inc - Portable native system C ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify shared system builtins without enabling long-mode-only contracts.
//===----------------------------------------------------------------------===//

TEST(X86PortableSystemSource, WrmsrnsUsesNativeCallsInBothCodeModes) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler) ||
      !std::getenv("NEVERD_TEST_NEVERC_LLC"))
    GTEST_SKIP() << "requires NeverC and its full i386 LLVM backend";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-wrmsrns-c", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0, ExportIndex = 0;
  for (Arch A : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      if (A == Arch::X86 && Format != BinaryFormat::ELF)
        continue;
      for (bool NoOpt : {false, true})
        for (uint32_t Value : {0u, 0x80000001u, 0xffffffffu}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          SCOPED_TRACE(Value);
          std::vector<uint8_t> Bytes;
          for (uint8_t Opcode : {0xb8, 0xb9, 0xba}) {
            Bytes.push_back(Opcode);
            uint32_t Word = Opcode == 0xb8 ? Value : ~Value;
            for (unsigned I = 0; I != sizeof(Word); ++I)
              Bytes.push_back(uint8_t(Word >> (I * 8)));
          }
          const size_t Offset = Bytes.size();
          Bytes.insert(Bytes.end(), {0x0f, 0x01, 0xc6, 0x31, 0xc0, 0xc3});
          auto Image = image(A, Format, Bytes);
          std::string High, LLVM;
          sourceRoutes(Image, NoOpt, Entry + Offset, "wrmsrns", &High, &LLVM);
          if (::testing::Test::HasFailure())
            return;
          if (A == Arch::X86) {
            const auto Stem = "wrmsrns32_" + std::to_string(ExportIndex++);
            saveX86SourceArtifact(
                Stem + ".bin",
                {reinterpret_cast<const char *>(Bytes.data()), Bytes.size()});
            saveX86SourceArtifact(Stem + ".high.c", freestandingC(High));
            saveX86SourceArtifact(Stem + ".llvm.c", freestandingC(LLVM));
          }
          for (const auto &Source : {High, LLVM}) {
            EXPECT_NE(Source.find("NEVERD_X86_WRMSRNS("), std::string::npos);
            compileNativeC(Compiler, Directory, Format, Source,
                           "llvm.x86.wrmsrns", Cases, std::nullopt, true, true,
                           A);
            if (::testing::Test::HasFailure())
              return;
          }
        }
    }
  EXPECT_EQ(Cases, 96u);
  RecordProperty("native_c_compilations", Cases);
}
