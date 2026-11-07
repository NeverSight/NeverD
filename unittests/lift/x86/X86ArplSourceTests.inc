//===- X86ArplSourceTests.inc - Native ARPL source contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve conditional word writes and real status through both C writers.
//===----------------------------------------------------------------------===//

TEST(X86ArplSource, RegisterMemorySegmentsAndFormatsUseNativeBuiltins) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool NoOpt : {false, true})
      for (unsigned Kind = 0; Kind != 4; ++Kind) {
        // cdecl destination, source; memory variants use destination as offset.
        std::vector<uint8_t> Bytes{0x8b, 0x44, 0x24, 4, 0x8b, 0x4c, 0x24, 8};
        if (Kind > 1)
          Bytes.push_back(Kind == 2 ? 0x64 : 0x65);
        Bytes.insert(Bytes.end(), {0x63, uint8_t(Kind ? 8 : 0xc8)});
        if (!Kind)
          Bytes.insert(Bytes.end(), {0x0f, 0xb7, 0xc0});
        // Return ZF in bit 16, and the adjusted selector for the register form.
        Bytes.insert(Bytes.end(),
                     {0x0f, 0x94, 0xc2, 0x0f, 0xb6, 0xd2, 0xc1, 0xe2, 16});
        if (Kind)
          Bytes.insert(Bytes.end(), {0x89, 0xd0});
        else
          Bytes.insert(Bytes.end(), {0x09, 0xd0});
        Bytes.push_back(0xc3);
        auto Image = image(Arch::X86, Format, Bytes);
        std::string High, C, Native;
        std::vector<uint8_t> Object;
        ASSERT_NO_THROW(sourceRoutes(Image, NoOpt, Entry, {}, &High, &C, {}, {},
                                     &Native, &Object, 4));
        ASSERT_FALSE(testing::Test::HasFailure());
        const std::string Name = Kind > 1 ? "arpl.mem.address32"
                                 : Kind   ? "arpl.mem"
                                          : "arpl.reg";
        const auto Plan = x86SystemStateNative(
            Kind ? Intrinsic::X86AdjustRplMemory : Intrinsic::X86AdjustRpl,
            Arch::X86, 2,
            Kind == 2   ? NdMemoryAddressSpace::X86FS
            : Kind == 3 ? NdMemoryAddressSpace::X86GS
                        : NdMemoryAddressSpace::Default);
        ASSERT_TRUE(Plan);
        for (const auto &Source : {High, C}) {
          EXPECT_EQ(Source.find("#include <immintrin.h>"), std::string::npos)
              << Source;
          EXPECT_NE(Source.find("NEVERD_X86_ARPL(({ "), std::string::npos)
              << Source;
          EXPECT_NE(Source.find(Plan->Builtin + "("), std::string::npos)
              << Source;
        }
        // Earlier ARPL declarations do not imply DS-safe memory lowering.
        if (llvm::Intrinsic::lookupIntrinsicID("llvm.x86.arpl.mem.address32")) {
          EXPECT_NE(Native.find("call " +
                                std::string(Kind ? "i8" : "{ i16, i8 }") +
                                " @llvm.x86." + Name),
                    std::string::npos)
              << Native;
          EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
        }
        const auto Stem = "arpl_" + std::to_string(unsigned(Format)) + "_" +
                          std::to_string(NoOpt) + "_" + std::to_string(Kind);
        saveX86SourceArtifact(Stem + ".high.c", High);
        saveX86SourceArtifact(Stem + ".llvm.c", C);
        saveX86SourceArtifact(Stem + ".ll", Native);
        saveX86SourceArtifact(
            Stem + ".o",
            {reinterpret_cast<const char *>(Object.data()), Object.size()});
      }
}

TEST(X86ArplSource, ExplicitAddressContractsKeepTheSourceAndBothOutputs) {
  for (unsigned Bits : {16u, 32u})
    for (unsigned Prefix : {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u}) {
      const auto Plan =
          x86SystemStateNative(Intrinsic::X86AdjustRplMemory, Arch::X86, 2,
                               NdMemoryAddressSpace::Default,
                               X86SystemStateNativeAddress{Bits, Prefix});
      ASSERT_TRUE(Plan);
      EXPECT_EQ(Plan->LLVMName, "llvm.x86.arpl.mem.address32");
      EXPECT_EQ(Plan->Builtin, "__builtin_ia32_arpl_mem_address32");
      EXPECT_EQ(Plan->InputBytes, (std::vector<uint16_t>{4, 2, 4, 4}));
      EXPECT_EQ(Plan->Assembly.InputConstraints.size(), 2u);
      EXPECT_NE(Plan->Assembly.Text.find("arpl $2, "), std::string::npos);
    }
  EXPECT_FALSE(x86SystemStateNative(Intrinsic::X86AdjustRpl, Arch::X64, 2,
                                    NdMemoryAddressSpace::Default));
}

TEST(X86ArplSource, NativeCallsRejectWeakenedEffectsAndInvalidControls) {
  const auto ID =
      llvm::Intrinsic::lookupIntrinsicID("llvm.x86.arpl.mem.address32");
  if (!ID)
    GTEST_SKIP() << "SDK predates native ARPL addresses";
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    llvm::LLVMContext Context;
    llvm::Module Module("arpl", Context);
    auto *Void = llvm::Type::getVoidTy(Context);
    auto *F = llvm::Function::Create(llvm::FunctionType::get(Void, false),
                                     llvm::GlobalValue::ExternalLinkage,
                                     "probe", Module);
    auto *B = llvm::BasicBlock::Create(Context, "entry", F);
    llvm::IRBuilder<> Builder(B);
    auto *Call =
        Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(&Module, ID),
                           {Builder.getInt32(0x1000), Builder.getInt16(3),
                            Builder.getInt32(Mutation == 1 ? 64 : 16),
                            Builder.getInt32(Mutation == 2 ? 0x66 : 0x64)});
    Builder.CreateRetVoid();
    if (Mutation == 3)
      Call->setMemoryEffects(llvm::MemoryEffects::readOnly());
    if (Mutation == 4)
      Call->setMemoryEffects(
          llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef));
    if (Mutation == 5)
      Call->addFnAttr(llvm::Attribute::Speculatable);
    if (Mutation == 6)
      Call->addFnAttr(llvm::Attribute::NoReturn);
    if (Mutation == 7)
      Call->setCallingConv(llvm::CallingConv::Fast);
    auto Render = [&] {
      return renderX86SystemStateNativeCall(
          Mutation == 8 ? Arch::X64 : Arch::X86, *Call, "result", true,
          {"p", "s", "16", "100"});
    };
    if (Mutation)
      EXPECT_THROW(Render(), std::invalid_argument);
    else {
      const auto Text = Render();
      ASSERT_TRUE(Text);
      EXPECT_NE(Text->find("__builtin_ia32_arpl_mem_address32"),
                std::string::npos);
      EXPECT_NE(Text->find("arpl %2, %%fs:(%w1)"), std::string::npos) << *Text;
    }
  }
}
