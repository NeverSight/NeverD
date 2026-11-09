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

TEST(X86PortableSystemSource, TlbControlsRetainEveryImplicitInput) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler) ||
      !std::getenv("NEVERD_TEST_NEVERC_LLC"))
    GTEST_SKIP() << "requires NeverC and its full i386 LLVM backend";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-tlb-c", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0, ExportIndex = 0;
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      if (A == Arch::X86 && Format != BinaryFormat::ELF)
        continue;
      for (const bool NoOpt : {false, true})
        for (const uint64_t Value :
             {UINT64_C(0), UINT64_C(1), UINT64_C(0x8000000180000041)})
          for (unsigned Kind = 0; Kind != 3; ++Kind) {
            if (Kind == 2 && A == Arch::X86)
              continue;
            SCOPED_TRACE(static_cast<unsigned>(A));
            SCOPED_TRACE(static_cast<unsigned>(Format));
            SCOPED_TRACE(NoOpt);
            SCOPED_TRACE(Value);
            SCOPED_TRACE(Kind);
            std::vector<uint8_t> Bytes;
            if (A == Arch::X64)
              Bytes.push_back(0x48);
            Bytes.push_back(0xb8);
            for (unsigned I = 0; I != (A == Arch::X64 ? 8u : 4u); ++I)
              Bytes.push_back(uint8_t(Value >> (I * 8)));
            for (const uint8_t Opcode : {0xb9, 0xba}) {
              Bytes.push_back(Opcode);
              const uint32_t Word =
                  Opcode == 0xb9 ? ~uint32_t(Value) : uint32_t(Value >> 32);
              for (unsigned I = 0; I != sizeof(Word); ++I)
                Bytes.push_back(uint8_t(Word >> (I * 8)));
            }
            const size_t Offset = Bytes.size();
            if (Kind == 2)
              Bytes.push_back(0x67);
            Bytes.insert(Bytes.end(),
                         {0x0f, 0x01, uint8_t(Kind == 0 ? 0xff : 0xfe), 0x31,
                          0xc0, 0xc3});
            const char *Mnemonic = Kind == 0 ? "tlbsync" : "invlpgb";
            const std::string API =
                Kind == 0        ? A == Arch::X64 ? "llvm.x86.tlbsync.control64"
                                                  : "llvm.x86.tlbsync.control32"
                : Kind == 2      ? "llvm.x86.invlpgb.addr32"
                : A == Arch::X64 ? "llvm.x86.invlpgb"
                                 : "llvm.x86.invlpgb32";
            auto Image = image(A, Format, Bytes);
            std::string High, LLVM;
            sourceRoutes(Image, NoOpt, Entry + Offset, Mnemonic, &High, &LLVM);
            if (::testing::Test::HasFailure())
              return;
            if (Format == BinaryFormat::ELF) {
              const auto Stem =
                  std::string(A == Arch::X64 ? "tlb64_" : "tlb32_") +
                  std::to_string(ExportIndex++);
              saveX86SourceArtifact(
                  Stem + ".bin",
                  {reinterpret_cast<const char *>(Bytes.data()), Bytes.size()});
              saveX86SourceArtifact(Stem + ".high.c", freestandingC(High));
              saveX86SourceArtifact(Stem + ".llvm.c", freestandingC(LLVM));
            }
            for (const auto &Source : {High, LLVM}) {
              compileNativeC(Compiler, Directory, Format, Source, API, Cases,
                             std::nullopt, true, true, A);
              if (::testing::Test::HasFailure())
                return;
            }
          }
    }
  EXPECT_EQ(Cases, 264u);
  RecordProperty("native_c_compilations", Cases);
}

TEST(X86PortableSystemSource, TlbSyncRejectsMissingAndNarrowedControls) {
  for (const auto A : {Arch::X86, Arch::X64}) {
    const uint16_t Word = A == Arch::X64 ? 8 : 4;
    for (const uint16_t Width : {0, 2, 4, 8}) {
      const auto Layout = x86SystemStateLayout(Intrinsic::X86TlbSync, A, Width);
      ASSERT_EQ(bool(Layout), Width == Word);
      if (Layout) {
        EXPECT_EQ(Layout->Arguments, 1u);
        EXPECT_EQ(Layout->InputSizes[0], Word);
        EXPECT_EQ(Layout->OutputSize, 0u);
      }
    }
  }
}

TEST(X86PortableSystemSource, LegacyTlbSyncSelectsCpuOnlyControl) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler) ||
      !std::getenv("NEVERD_TEST_NEVERC_LLC"))
    GTEST_SKIP() << "requires NeverC and its full i386 LLVM backend";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-tlbsync-legacy", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (const auto A : {Arch::X86, Arch::X64}) {
    llvm::LLVMContext Context;
    llvm::Module Module("legacy-tlbsync", Context);
    llvm::IRBuilder<> Builder(Context);
    auto *Signature = llvm::FunctionType::get(Builder.getVoidTy(), false);
    auto *Callee =
        llvm::Function::Create(Signature, llvm::GlobalValue::ExternalLinkage,
                               "llvm.x86.tlbsync", Module);
    auto *Function =
        llvm::Function::Create(Signature, llvm::GlobalValue::ExternalLinkage,
                               "legacy_tlbsync", Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Call = Builder.CreateCall(Callee);
    Builder.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    auto Render = [&]() {
      return renderX86SystemStateNativeCall(A, *Call, "", false, {});
    };
    const auto Text = Render();
    ASSERT_TRUE(Text);
    const std::string Bits = A == Arch::X64 ? "64" : "32";
    EXPECT_NE(Text->find("__builtin_ia32_tlbsync_control" + Bits + "((uint" +
                         Bits + "_t)(0))"),
              std::string::npos)
        << *Text;
    EXPECT_NE(Text->find("\"a\""), std::string::npos) << *Text;
    CEmitterOptions Options;
    Options.TheArch = A;
    Options.Format = BinaryFormat::ELF;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
    compileNativeC(Compiler, Directory, BinaryFormat::ELF, Source,
                   "llvm.x86.tlbsync.control" + Bits, Cases, std::nullopt, true,
                   true, A);
    if (::testing::Test::HasFailure())
      return;
    EXPECT_THROW((void)renderX86SystemStateNativeCall(A, *Call, "", true, {}),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)renderX86SystemStateNativeCall(A, *Call, "", false, {"0"}),
        std::invalid_argument);
    EXPECT_THROW((void)renderX86SystemStateNativeCall(Arch::AArch64, *Call, "",
                                                      false, {}),
                 std::invalid_argument);
    for (const auto Effects :
         {llvm::MemoryEffects::none(), llvm::MemoryEffects::readOnly(),
          llvm::MemoryEffects::writeOnly(), llvm::MemoryEffects::argMemOnly(),
          llvm::MemoryEffects::inaccessibleMemOnly(),
          llvm::MemoryEffects::inaccessibleOrArgMemOnly()}) {
      Call->setMemoryEffects(Effects);
      EXPECT_THROW(Render(), std::invalid_argument);
    }
    Call->setMemoryEffects(llvm::MemoryEffects::unknown());
    Call->addFnAttr(llvm::Attribute::Speculatable);
    EXPECT_THROW(Render(), std::invalid_argument);
    Call->removeFnAttr(llvm::Attribute::Speculatable);
    Call->setDoesNotReturn();
    EXPECT_THROW(Render(), std::invalid_argument);
    Call->removeFnAttr(llvm::Attribute::NoReturn);
    Call->setCallingConv(llvm::CallingConv::Fast);
    EXPECT_THROW(Render(), std::invalid_argument);
    Call->setCallingConv(llvm::CallingConv::C);
    llvm::IRBuilder<> Definition(
        llvm::BasicBlock::Create(Context, "entry", Callee));
    Definition.CreateRetVoid();
    EXPECT_THROW(Render(), std::invalid_argument);
  }
  EXPECT_EQ(Cases, 4u);
  RecordProperty("native_c_compilations", Cases);
}
