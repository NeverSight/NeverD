//===- X86VMXSourceTests.inc - Native i386 VMX projection -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both C routes retain mode-sized VMCS words and instruction-owned memory.
//===----------------------------------------------------------------------===//

TEST(X86VirtualizationSource, VmxoffRetainsFlagsAndTheLegacyVoidApi) {
  const auto Legacy = llvm::Intrinsic::lookupIntrinsicID("llvm.x86.vmxoff");
  RecordProperty("legacy_void_intrinsic_available", Legacy ? 1 : 0);
  for (const auto A : {Arch::X86, Arch::X64}) {
    const auto Plan = x86VirtualizationNative(Intrinsic::VmxOff, A,
                                              NdMemoryAddressSpace::Default);
    ASSERT_TRUE(Plan);
    EXPECT_EQ(Plan->LLVMName, "llvm.x86.vmxoff.status");
    EXPECT_EQ(Plan->Builtin, "__builtin_ia32_vmxoff_status");
    EXPECT_EQ(Plan->Result, X86VirtualizationResult::Flags);

    llvm::LLVMContext Context;
    llvm::Module Module("legacy-vmxoff", Context);
    llvm::IRBuilder<> Builder(Context);
    auto *Type = llvm::FunctionType::get(Builder.getVoidTy(), false);
    auto *Owner = llvm::Function::Create(Type, llvm::Function::ExternalLinkage,
                                         "owner", Module);
    Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Owner));
    auto *Callee = Legacy
                       ? llvm::Intrinsic::getOrInsertDeclaration(&Module, Legacy)
                       : llvm::Function::Create(
                             Type, llvm::Function::ExternalLinkage,
                             "llvm.x86.vmxoff", Module);
    auto *Call = Builder.CreateCall(Callee);
    Builder.CreateRetVoid();
    if (!Legacy) {
      EXPECT_THROW(renderX86VirtualizationNativeCall(A, *Call, {}, false, {}),
                   std::invalid_argument);
      continue;
    }
    auto Text = renderX86VirtualizationNativeCall(A, *Call, {}, false, {});
    ASSERT_TRUE(Text);
    EXPECT_NE(Text->find("__builtin_ia32_vmxoff("), std::string::npos);
    EXPECT_EQ(Text->find("__builtin_ia32_vmxoff_status("), std::string::npos);
    Call->setOnlyReadsMemory();
    EXPECT_THROW(renderX86VirtualizationNativeCall(A, *Call, {}, false, {}),
                 std::invalid_argument);
  }
}

TEST(X86VirtualizationSource, NeverCI386CompilesBothRoutesWithoutAssembly) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  const char *Backend = std::getenv("NEVERD_TEST_NEVERC_LLC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler) || !Backend ||
      !llvm::sys::fs::exists(Backend))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC and NEVERD_TEST_NEVERC_LLC";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-native-vmx32-c", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (bool NoOpt : {false, true})
    for (const auto &E : nativeVirtualizationEncodings(Arch::X86)) {
      const auto C = x86VirtualizationContract(E.Id);
      for (const auto Space :
           {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
            NdMemoryAddressSpace::X86GS}) {
        const auto Plan = x86VirtualizationNative(
            E.Id, Arch::X86, Space, physicalAddressBytes(E, Arch::X86));
        if (!Plan || Plan->Builtin.empty())
          continue;
        SCOPED_TRACE(Plan->LLVMName);
        SCOPED_TRACE(NoOpt);
        auto Bytes = E.Bytes;
        if (Space != NdMemoryAddressSpace::Default)
          Bytes.insert(Bytes.begin(), Plan->SegmentPrefix);
        size_t Offset;
        auto FunctionBytes = function(Arch::X86, Bytes, Offset);
        // Initialize the complete m128 descriptor for original-byte witnesses.
        for (uint8_t Displacement : {4, 8, 12}) {
          const uint8_t Store[] = {0xc7, 0x44, 0x24, Displacement,
                                   0x78, 0x56, 0x34, 0x12};
          FunctionBytes.insert(FunctionBytes.begin() + Offset,
                               std::begin(Store), std::end(Store));
          Offset += sizeof(Store);
        }
        auto Image = image(Arch::X86, BinaryFormat::ELF, FunctionBytes);
        std::string High, LLVM;
        sourceRoutes(Image, NoOpt, Entry + Offset, C.Mnemonic, &High, &LLVM);
        if (::testing::Test::HasFailure())
          return;
        const auto Name = "vmx32_" + Plan->Builtin.substr(15) + "_" +
                          std::to_string(NoOpt) + "_" +
                          std::to_string(unsigned(Space));
        saveX86SourceArtifact(Name + ".high.c", High);
        saveX86SourceArtifact(Name + ".llvm.c", LLVM);
        saveX86SourceArtifact(Name + ".bin", {reinterpret_cast<const char *>(
                                                  FunctionBytes.data()),
                                              FunctionBytes.size()});
        for (const auto &Source : {High, LLVM}) {
          EXPECT_NE(Source.find("#define NEVERD_X86_VIRTUALIZATION("),
                    std::string::npos)
              << Source;
          EXPECT_NE(Source.find(Plan->Builtin + "("), std::string::npos)
              << Source;
          compileNativeC(Compiler, Directory, BinaryFormat::ELF, Source,
                         Plan->LLVMName, Cases, std::nullopt, true, true,
                         Arch::X86);
          if (::testing::Test::HasFailure())
            return;
        }
      }
    }
  EXPECT_GT(Cases, 0u);
  RecordProperty("native_c_compilations", Cases);
}
