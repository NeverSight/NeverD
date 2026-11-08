//===- X86SVMSourceTests.inc - Native i386 SVM address widths ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Complete C exports keep AX/EAX addressing and the independent ECX ASID.
//===----------------------------------------------------------------------===//

TEST(X86VirtualizationSource, Address16AssemblyRequiresExactRegisterInputs) {
  for (const auto *Mnemonic : {"vmload", "vmsave", "invlpga"})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(Mnemonic);
      SCOPED_TRACE(Mutation);
      const bool Invalidate = llvm::StringRef(Mnemonic) == "invlpga";
      llvm::LLVMContext Context;
      llvm::Module Module("svm-address16", Context);
      std::vector<llvm::Type *> Types = {
          llvm::IntegerType::get(Context, Mutation == 1                  ? 32
                                          : Mutation == 2 && !Invalidate ? 64
                                                                         : 16)};
      if (Invalidate)
        Types.push_back(
            llvm::IntegerType::get(Context, Mutation == 2 ? 16 : 32));
      auto *Type =
          llvm::FunctionType::get(llvm::Type::getVoidTy(Context), Types, false);
      auto *Function = llvm::Function::Create(
          Type, llvm::Function::ExternalLinkage, "test", Module);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      std::vector<llvm::Value *> Operands;
      for (auto &Argument : Function->args())
        Operands.push_back(&Argument);
      std::string Constraints = Mutation == 5 ? "{bx}" : "{ax}";
      if (Invalidate)
        Constraints += ",{cx}";
      Constraints += ",~{memory}";
      auto *Assembly = llvm::InlineAsm::get(
          Type, "addr16 " + std::string(Mnemonic), Constraints, Mutation != 3);
      auto *Call = Builder.CreateCall(Type, Assembly, Operands);
      if (Mutation == 4)
        Call->setDoesNotAccessMemory();
      Builder.CreateRetVoid();
      std::vector<std::string> Arguments = {"address"};
      if (Invalidate)
        Arguments.push_back("asid");
      auto Render = [&] {
        return renderX86VirtualizationCall(
            Mutation == 6 ? Arch::X64 : Arch::X86, *Call, {}, false, Arguments);
      };
      if (Mutation) {
        EXPECT_THROW(Render(), std::invalid_argument);
      } else {
        const auto Source = Render();
        ASSERT_TRUE(Source);
        EXPECT_NE(Source->find("__builtin_ia32_" + std::string(Mnemonic) +
                               "_addr16("),
                  std::string::npos);
        EXPECT_NE(Source->find("addr16 " + std::string(Mnemonic)),
                  std::string::npos);
      }
    }
}

TEST(X86VirtualizationSource, NeverCI386SvmKeepsDefaultAndAddress16Forms) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  const char *Backend = std::getenv("NEVERD_TEST_NEVERC_LLC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler) || !Backend ||
      !llvm::sys::fs::exists(Backend))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC and NEVERD_TEST_NEVERC_LLC";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-native-svm32-c", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (bool NoOpt : {false, true})
    for (const auto &E : nativeVirtualizationEncodings()) {
      const auto C = x86VirtualizationContract(E.Id);
      if (!C.PhysicalAddress)
        continue;
      const auto Plan = x86VirtualizationNative(
          E.Id, Arch::X86, NdMemoryAddressSpace::Default,
          physicalAddressBytes(E, Arch::X86));
      ASSERT_TRUE(Plan);
      SCOPED_TRACE(Plan->LLVMName);
      SCOPED_TRACE(NoOpt);
      // Distinct upper halves expose AX/EAX and ECX/CX substitution. The
      // original accumulator remains live after the instruction.
      std::vector<uint8_t> Bytes = {0xb8, 0x56, 0x34, 0xb2, 0xa1,
                                    0xb9, 0xef, 0xcd, 0xab, 0x89};
      const auto Offset = Bytes.size();
      Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
      Bytes.insert(Bytes.end(), {0x31, 0xc8, 0xc3});
      auto Image = image(Arch::X86, BinaryFormat::ELF, Bytes);
      std::string High, LLVM;
      sourceRoutes(Image, NoOpt, Entry + Offset, C.Mnemonic, &High, &LLVM);
      if (::testing::Test::HasFailure())
        return;
      const auto Name =
          "svm32_" + Plan->Builtin.substr(15) + "_" + std::to_string(NoOpt);
      saveX86SourceArtifact(Name + ".high.c", High);
      saveX86SourceArtifact(Name + ".llvm.c", LLVM);
      saveX86SourceArtifact(
          Name + ".bin",
          {reinterpret_cast<const char *>(Bytes.data()), Bytes.size()});
      for (const auto &Source : {High, LLVM}) {
        EXPECT_NE(Source.find(Plan->Builtin + "("), std::string::npos)
            << Source;
        compileNativeC(Compiler, Directory, BinaryFormat::ELF, Source,
                       Plan->LLVMName, Cases, std::nullopt, true, true,
                       Arch::X86);
        if (::testing::Test::HasFailure())
          return;
      }
    }
  EXPECT_EQ(Cases, 48u);
  RecordProperty("native_c_compilations", Cases);
}
