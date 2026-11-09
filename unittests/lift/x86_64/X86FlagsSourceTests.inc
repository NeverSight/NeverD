//===- X86FlagsSourceTests.inc - Native FLAGS projection -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original stack flags retain their image width through both C routes.
//===----------------------------------------------------------------------===//

namespace {
BinaryImage flagsFunction(Arch A, BinaryFormat Format, bool Word, bool Write) {
  std::vector<uint8_t> Bytes;
  if (Write) {
    if (A == Arch::X86)
      Bytes = {0x8b, 0x44, 0x24, 4};
    else
      Bytes = {0x48, 0x89, uint8_t(Format == BinaryFormat::COFF ? 0xc8 : 0xf8)};
    if (Word)
      Bytes.push_back(0x66);
    Bytes.push_back(0x50);
    if (Word)
      Bytes.push_back(0x66);
    Bytes.insert(Bytes.end(), {0x9d, 0xb8, 0, 0, 0, 0, 0xc3});
  } else {
    if (Word)
      Bytes.push_back(0x66);
    Bytes.push_back(0x9c);
    if (Word)
      Bytes.push_back(0x66);
    Bytes.push_back(0x58);
    if (Word)
      Bytes.insert(Bytes.end(), {0x0f, 0xb7, 0xc0});
    Bytes.push_back(0xc3);
  }
  return image(A, Format, Bytes);
}

void compileFlagsForms(llvm::StringRef Compiler = {}) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    if (A == Arch::X86 && !Compiler.empty())
      continue; // NeverC compiles only the x64 source target.
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      for (bool NoOpt : {false, true})
        for (bool Word : {false, true})
          for (bool Write : {false, true}) {
            SCOPED_TRACE(testing::Message()
                         << unsigned(A) << " " << unsigned(Format) << " "
                         << NoOpt << " " << Word << " " << Write);
            auto Image = flagsFunction(A, Format, Word, Write);
            ASSERT_NO_FATAL_FAILURE(compileRoutes(Image, NoOpt, Entry, "",
                                                  std::nullopt, false, false,
                                                  {}, {}, 0, true, Compiler));
          }
    }
  }
}
} // namespace

TEST(X86FlagsSource, EveryWidthSurvivesBothRoutesAndFormats) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (bool Word : {false, true})
          for (bool Write : {false, true}) {
            SCOPED_TRACE(testing::Message()
                         << unsigned(A) << " " << unsigned(Format) << " "
                         << NoOpt << " " << Word << " " << Write);
            const auto P =
                x86FlagsNative(Write ? Intrinsic::Popf : Intrinsic::Pushf, A,
                               Word             ? 2
                               : A == Arch::X64 ? 8
                                                : 4);
            ASSERT_TRUE(P);
            auto Image = flagsFunction(A, Format, Word, Write);
            std::string High, C, Native;
            std::vector<uint8_t> Object;
            ASSERT_NO_THROW(sourceRoutes(Image, NoOpt, Entry, "", &High, &C, {},
                                         {}, &Native, &Object));
            ASSERT_FALSE(testing::Test::HasFailure());
            for (const auto &Source : {High, C}) {
              if (Word)
                EXPECT_NE(Source.find("NEVERD_X86_FLAGS16(" +
                                      std::string(P->Builtin) + "("),
                          std::string::npos)
                    << Source;
              else
                EXPECT_NE(Source.find(std::string(P->Builtin) + "("),
                          std::string::npos)
                    << Source;
            }
            if (llvm::Intrinsic::lookupIntrinsicID(P->LLVMName)) {
              EXPECT_NE(Native.find(P->LLVMName), std::string::npos) << Native;
              EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
            } else {
              EXPECT_NE(Native.find(Write ? "popfw" : "pushfw"),
                        std::string::npos)
                  << Native;
              EXPECT_EQ(Native.find(Write ? "popfq" : "pushfq"),
                        std::string::npos)
                  << Native;
            }
            ASSERT_FALSE(Object.empty());
            const auto Name =
                "flags_" + std::to_string(unsigned(A)) + "_" +
                std::to_string(unsigned(Format)) + "_" + std::to_string(NoOpt) +
                "_" + std::to_string(Word) + "_" + std::to_string(Write);
            saveX86SourceArtifact(Name + ".high.c", High);
            saveX86SourceArtifact(Name + ".llvm.c", C);
            saveX86SourceArtifact(Name + ".ll", Native);
            saveX86SourceArtifact(
                Name + ".o",
                {reinterpret_cast<const char *>(Object.data()), Object.size()});
          }
}

TEST(X86FlagsSource, NativeCallContractsRejectInventedWidthsAndEffects) {
  for (const auto &P : X86FlagsNativeInventory) {
    if (!llvm::Intrinsic::lookupIntrinsicID(P.LLVMName))
      continue;
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(testing::Message() << P.LLVMName << " " << Mutation);
      llvm::LLVMContext Context;
      llvm::Module Module("flags", Context);
      llvm::IRBuilder<> B(Context);
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), {}, false),
          llvm::Function::ExternalLinkage, "test", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Word = B.getIntNTy(8 * P.Bytes);
      std::vector<llvm::Type *> Types;
      std::vector<llvm::Value *> Args;
      if (P.write()) {
        Types.push_back(Mutation == 1 ? B.getInt8Ty() : Word);
        Args.push_back(llvm::ConstantInt::get(Types.back(), 2));
      }
      if (Mutation == 2) {
        Types.push_back(B.getInt32Ty());
        Args.push_back(B.getInt32(0));
      }
      llvm::Type *Result = P.write() ? B.getVoidTy() : Word;
      if (Mutation == 1 && !P.write())
        Result = B.getInt8Ty();
      auto *Type = llvm::FunctionType::get(Result, Types, Mutation == 3);
      auto *Callee = llvm::Function::Create(
          Type, llvm::Function::ExternalLinkage, P.LLVMName, Module);
      if (Mutation == 4)
        Callee->setMemoryEffects(llvm::MemoryEffects::none());
      if (Mutation == 5)
        Callee->addFnAttr(llvm::Attribute::Speculatable);
      if (Mutation == 6)
        Callee->addFnAttr(llvm::Attribute::NoReturn);
      if (Mutation == 7) {
        llvm::IRBuilder<> Definition(
            llvm::BasicBlock::Create(Context, "body", Callee));
        if (P.write())
          Definition.CreateRetVoid();
        else
          Definition.CreateRet(llvm::ConstantInt::get(Word, 0));
      }
      auto *Call = B.CreateCall(Callee, Args);
      if (Mutation == 8)
        Call->setCallingConv(llvm::CallingConv::Fast);
      const Arch Target = P.Bytes == 4 ? Arch::X86 : Arch::X64;
      const auto Render = [&] {
        return renderX86FlagsCall(
            Mutation == 9 ? Arch::AArch64 : Target, *Call, "flags", !P.write(),
            std::vector<std::string>(Args.size(), "image"));
      };
      if (Mutation)
        EXPECT_THROW(Render(), std::invalid_argument);
      else {
        auto R = Render();
        ASSERT_TRUE(R);
        EXPECT_NE(R->Code.find(P.Builtin), std::string::npos);
        if (!P.write()) {
          const auto Unused = renderX86FlagsCall(Target, *Call, "", false, {});
          ASSERT_TRUE(Unused);
          EXPECT_NE(Unused->Code.find(P.Builtin), std::string::npos);
        }
      }
    }
  }
}

TEST(X86FlagsSource, HighCallsRejectInventedWidthsAndEffects) {
  for (const auto &P : X86FlagsNativeInventory)
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(testing::Message() << P.LLVMName << " " << Mutation);
      HighExpr Call;
      Call.Kind = ExprKind::Call;
      Call.IntrinsicId = P.Id;
      Call.Type =
          P.write() ? NdType::makeVoid() : NdType::makeInt(P.Bytes, false);
      if (P.write()) {
        auto Input = std::make_shared<HighExpr>();
        Input->Kind = ExprKind::Const;
        Input->Type = NdType::makeInt(P.Bytes, false);
        Input->ConstVal = 2;
        Call.Operands.push_back(Input);
      }
      auto &ImageType = P.write() ? Call.Operands[0]->Type : Call.Type;
      if (Mutation == 1)
        ImageType = NdType::makeInt(1, false);
      if (Mutation == 2)
        ImageType = NdType::makeFloat(P.Bytes);
      if (Mutation == 3)
        Call.DoesNotReturn = true;
      if (Mutation == 4)
        Call.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      if (Mutation == 5)
        Call.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 6)
        Call.Operands.push_back(std::make_shared<HighExpr>());
      if (Mutation == 7)
        Call.Type = P.write() ? NdType::makeInt(2, false) : NdType::makeVoid();
      bool Intrinsics = false;
      const auto Render = [&] {
        return renderX86TypedIntrinsicCall(
            P.Bytes == 4 ? Arch::X86 : Arch::X64, Call,
            [](const HighExpr &) { return "image"; }, Intrinsics, true);
      };
      if (Mutation)
        EXPECT_THROW(Render(), std::invalid_argument);
      else
        EXPECT_NE(Render().find(P.Builtin), std::string::npos);
    }
}

TEST(X86FlagsSource, LegacyAssemblyRequiresExactWidthAndTarget) {
  for (const auto &P : X86FlagsNativeInventory)
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(testing::Message() << P.LLVMName << " " << Mutation);
      const Arch Target = P.Bytes == 4 ? Arch::X86 : Arch::X64;
      const auto Render = [&] {
        return renderX86InlineAsm(
            Mutation == 1 ? Arch::AArch64 : Target, P.Assembly, "",
            Mutation == 2, Mutation == 3 ? "" : "flags",
            !P.write() || Mutation == 3,
            std::vector<std::string>(unsigned(P.write()) + (Mutation == 4),
                                     "image"));
      };
      if (Mutation)
        EXPECT_THROW(Render(), std::invalid_argument);
      else
        EXPECT_NE(Render().Code.find(P.Builtin), std::string::npos);
    }
}

TEST(X86FlagsSource, StockClangCompilesBothRoutesAndAllFormats) {
  compileFlagsForms();
  RecordProperty("c_compilations", 192);
}

TEST(X86FlagsSource, NeverCCompilesBothNativeRoutesAndSupportedFormats) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  compileFlagsForms(Compiler);
  RecordProperty("c_compilations", 96);
}
