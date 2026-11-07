//===- X86SegmentMoveSourceTests.inc - Native selector source routes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Segment moves share the pipeline/compilation fixture with system sources.
//===----------------------------------------------------------------------===//

TEST(X86SegmentMoveSource, EverySelectorSurvivesBothRoutesAndFormats) {
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (unsigned Segment = 0; Segment != 6; ++Segment)
          for (bool Read : {false, true}) {
            if (!Read && Segment == 1)
              continue;
            SCOPED_TRACE(testing::Message()
                         << unsigned(A) << " " << unsigned(Format) << " "
                         << NoOpt << " " << Segment << " " << Read);
            std::vector<uint8_t> Code = {0x31, 0xc0};
            if (Read)
              Code.insert(Code.end(), {0x8c, uint8_t(0xc0 | (Segment << 3))});
            else
              Code.insert(Code.end(), {0x8e, uint8_t(0xc0 | (Segment << 3))});
            Code.push_back(0xc3);
            auto Image = image(A, Format, Code);
            std::string High, C, Native;
            std::vector<uint8_t> Object;
            sourceRoutes(Image, NoOpt, Entry + 2, "", &High, &C, {}, {},
                         &Native, &Object);
            ASSERT_FALSE(testing::Test::HasFailure());
            const auto &P = X86SegmentMoveInventory[Read ? 0 : 2];
            for (const auto &Source : {High, C}) {
              EXPECT_NE(Source.find(P.Builtin), std::string::npos) << Source;
              EXPECT_NE(Source.find("#define NEVERD_X86_SEGMENT_MOVE"),
                        std::string::npos)
                  << Source;
            }
            if (llvm::Intrinsic::lookupIntrinsicID(P.LLVMName)) {
              EXPECT_NE(Native.find(P.LLVMName), std::string::npos) << Native;
              EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
            }
            ASSERT_FALSE(Object.empty());
            const auto Name =
                "segment_" + std::to_string(unsigned(A)) + "_" +
                std::to_string(unsigned(Format)) + "_" + std::to_string(NoOpt) +
                "_" + std::to_string(Segment) + "_" + std::to_string(Read);
            saveX86SourceArtifact(Name + ".high.c", High);
            saveX86SourceArtifact(Name + ".llvm.c", C);
            saveX86SourceArtifact(Name + ".ll", Native);
            saveX86SourceArtifact(
                Name + ".o",
                {reinterpret_cast<const char *>(Object.data()), Object.size()});
          }
}

TEST(X86SegmentMoveSource, NativeCallsValidateRegistersWidthsAndMemoryForms) {
  for (const auto &P : X86SegmentMoveInventory)
    for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
      if (Mutation == 2 && P.Read)
        continue;
      if (Mutation == 13 && (!P.Read || P.Memory))
        continue;
      SCOPED_TRACE(testing::Message() << P.LLVMName << " " << Mutation);
      llvm::LLVMContext Context;
      llvm::Module Module("segments", Context);
      llvm::IRBuilder<> B(Context);
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), {B.getInt32Ty()}, false),
          llvm::Function::ExternalLinkage, "test", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
      std::vector<llvm::Value *> Args{Mutation == 1   ? B.getInt32(6)
                                      : Mutation == 2 ? B.getInt32(1)
                                                      : B.getInt32(2)};
      if (Mutation == 3)
        Args[0] = Function->getArg(0);
      if (P.Memory)
        Args.push_back(llvm::ConstantPointerNull::get(
            llvm::PointerType::get(Context, Mutation == 4 ? 256 : 0)));
      else if (!P.Read)
        Args.push_back(Mutation == 4 ? B.getInt64(0) : B.getInt16(0));
      else if (Mutation == 4)
        Args[0] = B.getInt16(2);
      std::vector<llvm::Type *> Types;
      for (auto *Arg : Args)
        Types.push_back(Arg->getType());
      auto *Return = P.Read && !P.Memory ? B.getInt16Ty() : B.getVoidTy();
      if (Mutation == 5)
        Return = B.getInt64Ty();
      auto *Type = llvm::FunctionType::get(Return, Types, Mutation == 7);
      auto *Callee = llvm::Function::Create(
          Type, llvm::Function::ExternalLinkage, P.LLVMName, Module);
      auto *Call = B.CreateCall(Callee, Args);
      if (Mutation == 8)
        Call->setMemoryEffects(llvm::MemoryEffects::none());
      if (Mutation == 9)
        Call->setMemoryEffects(llvm::MemoryEffects::readOnly());
      if (Mutation == 10)
        Call->setCallingConv(llvm::CallingConv::Fast);
      if (Mutation == 11)
        Call->addFnAttr(llvm::Attribute::NoReturn);
      if (Mutation == 12)
        Call->addFnAttr(llvm::Attribute::Speculatable);
      B.CreateRetVoid();
      const auto Render = [&] {
        return renderX86SegmentMoveCall(
            Mutation == 6 ? Arch::AArch64 : Arch::X64, *Call,
            Mutation == 13 ? "" : "result", true,
            std::vector<std::string>(Args.size(), "operand"));
      };
      if (Mutation)
        EXPECT_THROW(Render(), std::invalid_argument);
      else {
        const auto Source = Render();
        ASSERT_TRUE(Source);
        EXPECT_NE(Source->find(P.Builtin), std::string::npos);
        EXPECT_NE(Source->find("NEVERD_X86_SEGMENT_MOVE("), std::string::npos);
      }
    }
}

TEST(X86SegmentMoveSource, NativeRegisterAndMemoryCallsCompileAcrossFormats) {
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-segment-c", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Functions = 0, CCompilations = 0, NeverCCompilations = 0;
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      llvm::LLVMContext Context;
      llvm::Module Module("segment-moves", Context);
      llvm::IRBuilder<> B(Context);
      const std::string Triple =
          std::string(A == Arch::X64 ? "x86_64" : "i386") +
          (Format == BinaryFormat::COFF    ? "-pc-windows-msvc"
           : Format == BinaryFormat::MachO ? "-apple-darwin"
                                           : "-linux-gnu");
      Module.setTargetTriple(llvm::Triple(Triple));
      Module.setDataLayout(A == Arch::X64 ? "e-p:64:64-i64:64-i128:128"
                                          : "e-p:32:32-i64:64");
      for (const auto &P : X86SegmentMoveInventory) {
        auto *Return = P.Read && !P.Memory ? B.getInt16Ty() : B.getVoidTy();
        std::vector<llvm::Type *> Types{B.getInt32Ty()}, Parameters;
        if (P.Memory)
          Parameters.push_back(B.getPtrTy());
        else if (!P.Read)
          Parameters.push_back(B.getInt16Ty());
        Types.insert(Types.end(), Parameters.begin(), Parameters.end());
        auto *Callee = llvm::Function::Create(
            llvm::FunctionType::get(Return, Types, false),
            llvm::Function::ExternalLinkage, P.LLVMName, Module);
        for (unsigned Segment = 0; Segment != 6; ++Segment) {
          if (!P.Read && Segment == 1)
            continue;
          auto *Function = llvm::Function::Create(
              llvm::FunctionType::get(Return, Parameters, false),
              llvm::Function::ExternalLinkage,
              "segment_case_" + std::to_string(Functions++), Module);
          B.SetInsertPoint(
              llvm::BasicBlock::Create(Context, "entry", Function));
          std::vector<llvm::Value *> Args{B.getInt32(Segment)};
          if (!Parameters.empty())
            Args.push_back(Function->getArg(0));
          auto *Call = B.CreateCall(Callee, Args);
          if (Return->isVoidTy())
            B.CreateRetVoid();
          else
            B.CreateRet(Call);
        }
      }
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      const auto Name = "segment_calls_" + std::to_string(unsigned(A)) + "_" +
                        std::to_string(unsigned(Format));
      CEmitterOptions Options;
      Options.TheArch = A;
      Options.Format = Format;
      Options.PreserveLLVMFunctionTypes = true;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
      saveX86SourceArtifact(Name + ".llvm.c", Source);
      for (const auto &P : X86SegmentMoveInventory)
        EXPECT_NE(Source.find(P.Builtin), std::string::npos) << Source;
      if (llvm::Intrinsic::lookupIntrinsicID("llvm.x86.readseg")) {
        auto Copy = llvm::CloneModule(Module);
        const auto Object = Codegen().compile(*Copy, A, Format);
        ASSERT_TRUE(Object.Success);
        ASSERT_FALSE(Object.ObjectData.empty());
        saveX86SourceArtifact(Name + ".o", {reinterpret_cast<const char *>(
                                                Object.ObjectData.data()),
                                            Object.ObjectData.size()});
      }
      llvm::SmallString<128> Input(Directory), Output(Directory),
          Errors(Directory);
      llvm::sys::path::append(Input, "fallback.c");
      llvm::sys::path::append(Output, "fallback.o");
      llvm::sys::path::append(Errors, "fallback.err");
      std::error_code Error;
      llvm::raw_fd_ostream File(Input, Error);
      ASSERT_FALSE(Error);
      File << freestandingC(Source);
      File.close();
      const std::string Target = "--target=" + Triple;
      for (const char *Opt : {"-O0", "-O2"}) {
        SCOPED_TRACE(Triple);
        SCOPED_TRACE(Opt);
        const std::vector<llvm::StringRef> Arguments{
            Compiler,
            Target,
            "-std=gnu23",
            "-ffreestanding",
            "-c",
            Opt,
            "-Werror=implicit-function-declaration",
            Input,
            "-o",
            Output};
        const std::optional<llvm::StringRef> Redirects[]{
            std::nullopt, std::nullopt, Errors.str()};
        const auto Status = llvm::sys::ExecuteAndWait(Compiler, Arguments,
                                                      std::nullopt, Redirects);
        const auto Diagnostics = llvm::MemoryBuffer::getFile(Errors);
        ASSERT_EQ(Status, 0)
            << (Diagnostics ? (*Diagnostics)->getBuffer().str() : "") << Source;
        ++CCompilations;
      }
      if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
          A == Arch::X64 && NeverC && llvm::sys::fs::exists(NeverC))
        compileNativeC(NeverC, Directory, Format, Source,
                       "llvm.x86.readseg.mem", NeverCCompilations, std::nullopt,
                       true);
      ASSERT_FALSE(testing::Test::HasFailure());
    }
  EXPECT_EQ(Functions, 132u);
  EXPECT_EQ(CCompilations, 12u);
  RecordProperty("segment_functions", Functions);
  RecordProperty("segment_c_compilations", CCompilations);
  RecordProperty("segment_neverc_compilations", NeverCCompilations);
}

TEST(X86SegmentMoveSource, LegacyAssemblyRequiresTheExactContract) {
  for (const bool Read : {false, true})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      llvm::LLVMContext Context;
      llvm::Module Module("segment-assembly", Context);
      llvm::IRBuilder<> B(Context);
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), {}, false),
          llvm::Function::ExternalLinkage, "test", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
      const std::string Segment = Mutation == 7 ? "cs" : "ss";
      const auto Assembly =
          Read ? "mov %" + Segment + ", $0" : "mov ${0:w}, %" + Segment;
      std::vector<llvm::Value *> Args;
      std::vector<llvm::Type *> Types;
      if (!Read) {
        Types.push_back(Mutation == 5 ? B.getInt16Ty() : B.getInt32Ty());
        Args.push_back(llvm::ConstantInt::get(Types.back(), 0));
      }
      auto *Return = Read ? B.getInt16Ty() : B.getVoidTy();
      if (Mutation == 6)
        Return = B.getInt64Ty();
      auto *Type = llvm::FunctionType::get(Return, Types, false);
      const char *Constraints = Read ? "=r" : "r,~{memory}";
      if (Mutation == 1)
        Constraints = Read ? "=r,~{memory}" : "r";
      if (!Read && Mutation == 6)
        Constraints = "=r,r,~{memory}";
      auto *AssemblyValue = llvm::InlineAsm::get(
          Type, Assembly, Constraints, Mutation != 2, Mutation == 3,
          llvm::InlineAsm::AD_ATT, Mutation == 4);
      auto *Call = B.CreateCall(AssemblyValue, Args);
      B.CreateRetVoid();
      const auto Render = [&] {
        return renderX86SegmentMoveCall(
            Arch::X64, *Call, "result", true,
            std::vector<std::string>(Args.size(), "operand"));
      };
      if (!Mutation || (Read && (Mutation == 5 || Mutation == 7)))
        EXPECT_TRUE(Render());
      else
        EXPECT_THROW(Render(), std::invalid_argument);
    }
}

TEST(X86SegmentMoveSource,
     SelectorReadAndStackReloadExecuteThroughBothCompilers) {
#if defined(__linux__) && defined(__x86_64__)
  const char Driver[] = R"C(
extern void virtualization_test(unsigned short *);
int main(void) {
  unsigned short before, after, words[] = {0x1234, 0xffff, 0x5678};
  __asm__ volatile("mov %%ss, %0" : "=r"(before));
  virtualization_test(words + 1);
  __asm__ volatile("mov %%ss, %0" : "=r"(after));
  return before != after || words[1] != before ||
         words[0] != 0x1234 || words[2] != 0x5678;
}
)C";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const auto *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  for (const auto &Compiler : Compilers)
    for (bool NoOpt : {false, true}) {
      auto Image =
          image(Arch::X64, BinaryFormat::ELF,
                {0x8c, 0xd0, 0x8e, 0xd0, 0x8c, 0xd0, 0x66, 0x89, 0x07, 0xc3});
      compileRoutes(Image, NoOpt, Entry, "", std::nullopt, true, false, Driver,
                    {}, 0, false, Compiler);
      ASSERT_FALSE(testing::Test::HasFailure());
    }
#else
  GTEST_SKIP() << "Native x64 segment execution requires Linux x64";
#endif
}
