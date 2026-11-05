//===- X86HresetSourceTests.inc - Native history-reset source
//--------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain EAX and target-feature requirements through both source writers.
//===----------------------------------------------------------------------===//

TEST(X86HresetSource, NativeBuiltinAndFeaturesSurviveBothModesAndFormats) {
  for (auto Target : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (uint8_t Immediate : {0, 0xa5}) {
          std::vector<uint8_t> Bytes;
          if (Target == Arch::X86)
            Bytes = {0x8b, 0x44, 0x24, 4}; // eax <- cdecl input
          else
            Bytes = {0x89, uint8_t(Format == BinaryFormat::COFF ? 0xc8 : 0xf8)};
          const auto Offset = Bytes.size();
          Bytes.insert(Bytes.end(),
                       {0xf3, 0x0f, 0x3a, 0xf0, 0xc0, Immediate, 0xc3});
          auto Image = image(Target, Format, Bytes);
          std::string High, C, Native;
          std::vector<uint8_t> Object;
          ASSERT_NO_THROW(sourceRoutes(Image, NoOpt, Entry + Offset, "hreset",
                                       &High, &C, {}, {}, &Native, &Object, 4));
          ASSERT_FALSE(testing::Test::HasFailure());
          for (const auto &Source : {High, C}) {
            EXPECT_NE(Source.find("NEVERD_X86_HRESET(({ "), std::string::npos)
                << Source;
            EXPECT_NE(Source.find("__builtin_ia32_hreset((uint32_t)"),
                      std::string::npos)
                << Source;
            EXPECT_NE(Source.find("target(\"hreset\")"), std::string::npos)
                << Source;
            EXPECT_EQ(Source.find("#include <immintrin.h>"), std::string::npos)
                << Source;
          }
          if (llvm::Intrinsic::lookupIntrinsicID("llvm.x86.hreset")) {
            EXPECT_NE(Native.find("call void @llvm.x86.hreset(i32"),
                      std::string::npos)
                << Native;
            EXPECT_NE(Native.find("+hreset"), std::string::npos) << Native;
            EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
          }
          const auto Stem =
              "hreset_" + std::to_string(Target == Arch::X64 ? 64 : 32) + "_" +
              std::to_string(unsigned(Format)) + "_" + std::to_string(NoOpt) +
              "_" + std::to_string(Immediate);
          saveX86SourceArtifact(Stem + ".high.c", High);
          saveX86SourceArtifact(Stem + ".llvm.c", C);
          saveX86SourceArtifact(Stem + ".ll", Native);
          saveX86SourceArtifact(
              Stem + ".o",
              {reinterpret_cast<const char *>(Object.data()), Object.size()});
        }
}

TEST(X86HresetSource, NativeCallsRejectLostEffectsAndWrongMode) {
  const auto ID = llvm::Intrinsic::lookupIntrinsicID("llvm.x86.hreset");
  if (!ID)
    GTEST_SKIP() << "SDK predates native HRESET";
  for (auto Target : {Arch::X86, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      llvm::LLVMContext Context;
      llvm::Module Module("history", Context);
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
          llvm::GlobalValue::ExternalLinkage, "probe", Module);
      llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", F));
      auto *Call = B.CreateCall(
          llvm::Intrinsic::getOrInsertDeclaration(&Module, ID), B.getInt32(1));
      B.CreateRetVoid();
      if (Mutation == 1)
        Call->setMemoryEffects(llvm::MemoryEffects::none());
      if (Mutation == 2)
        Call->setMemoryEffects(
            llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef));
      if (Mutation == 3)
        Call->addFnAttr(llvm::Attribute::Speculatable);
      if (Mutation == 4)
        Call->setCallingConv(llvm::CallingConv::Fast);
      if (Mutation == 5)
        Call->setDoesNotReturn();
      auto Render = [&] {
        return renderX86SystemStateNativeCall(Mutation == 6 ? Arch::AArch64
                                                            : Target,
                                              *Call, "", false, {"input"});
      };
      if (Mutation)
        EXPECT_THROW(Render(), std::invalid_argument);
      else {
        const auto Text = Render();
        ASSERT_TRUE(Text);
        EXPECT_NE(Text->find("__builtin_ia32_hreset"), std::string::npos);
        EXPECT_NE(Text->find("hreset $0"), std::string::npos);
      }
    }
}
