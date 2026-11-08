//===- X86ScalarSystemSourceTests.inc - Native scalar MSR and port I/O --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both x86 code modes share scalar effects and complete C source
/// qualification.
//===----------------------------------------------------------------------===//

namespace {
struct ScalarSystemEncoding {
  X86ScalarSystemNative Plan;
  bool Immediate;
  bool Dead;
  size_t Offset;
  std::vector<uint8_t> Bytes;
};

std::vector<ScalarSystemEncoding> scalarSystemEncodings() {
  std::vector<ScalarSystemEncoding> Result;
  for (const auto &Plan : X86ScalarSystemNativeInventory)
    for (bool Immediate : {false, true}) {
      if (Immediate && !Plan.Port)
        continue;
      for (bool Dead : {false, true}) {
        if (Dead && Plan.Write)
          continue;
        std::vector<uint8_t> Bytes = {0xb8, 0x12, 0x34, 0x56, 0x87,
                                      0xb9, 0x00, 0x01, 0x00, 0xc0,
                                      0xba, 0x34, 0x12, 0x00, 0x00};
        const auto Offset = Bytes.size();
        if (Plan.Port) {
          if (Plan.Bytes == 2)
            Bytes.push_back(0x66);
          Bytes.push_back(uint8_t((Immediate ? 0xe4 : 0xec) + 2 * Plan.Write +
                                  (Plan.Bytes != 1)));
          if (Immediate)
            Bytes.push_back(0x34);
        } else {
          Bytes.insert(Bytes.end(), {0x0f, uint8_t(Plan.Write ? 0x30 : 0x32)});
        }
        if (Dead || Plan.Write)
          Bytes.insert(Bytes.end(), {0x31, 0xc0});
        Bytes.push_back(0xc3);
        Result.push_back({Plan, Immediate, Dead, Offset, std::move(Bytes)});
      }
    }
  return Result;
}
} // namespace

TEST_P(X86SystemRegisterAccess,
       ScalarMsrAndPortEffectsSurviveBothSourceRoutes) {
  const auto A = GetParam().TheArch;
  const auto Format = GetParam().Format;
  unsigned Fixtures = 0;
  for (const auto &E : scalarSystemEncodings()) {
    SCOPED_TRACE(E.Plan.LLVMName);
    SCOPED_TRACE(E.Immediate);
    SCOPED_TRACE(E.Dead);
    auto Image = image(A, Format, E.Bytes);
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(NoOpt);
      compileRoutes(Image, NoOpt, Entry + E.Offset, {}, std::nullopt, false,
                    A == Arch::X64 && Format == BinaryFormat::ELF, {}, {}, 0,
                    true);
      if (::testing::Test::HasFailure() || ::testing::Test::IsSkipped())
        return;
    }
    ++Fixtures;
  }
  EXPECT_EQ(Fixtures, 21u);
  RecordProperty("scalar_system_source_fixtures", Fixtures);
}

TEST_P(X86SystemRegisterAccess, ScalarMsrAndPortsUseNeverCNativeIntrinsics) {
  const auto A = GetParam().TheArch;
  if (A == Arch::X86 && GetParam().Format != BinaryFormat::ELF)
    GTEST_SKIP() << "NeverC i386 source ABI currently covers Linux ELF";
  if (A == Arch::X86 && !std::getenv("NEVERD_TEST_NEVERC_LLC"))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC_LLC to the full i386 LLVM backend";
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-scalar-system-c",
                                                    Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  unsigned ExportIndex = 0;
  for (bool NoOpt : {false, true})
    for (const auto &E : scalarSystemEncodings()) {
      SCOPED_TRACE(E.Plan.LLVMName);
      SCOPED_TRACE(E.Immediate);
      SCOPED_TRACE(E.Dead);
      SCOPED_TRACE(NoOpt);
      auto Image = image(A, GetParam().Format, E.Bytes);
      std::string High, LLVM, Native;
      sourceRoutes(Image, NoOpt, Entry + E.Offset, {}, &High, &LLVM, {}, {},
                   &Native);
      if (::testing::Test::HasFailure())
        return;
      if (llvm::Intrinsic::lookupIntrinsicID(E.Plan.LLVMName)) {
        EXPECT_NE(Native.find(std::string("@") + E.Plan.LLVMName + "("),
                  std::string::npos)
            << Native;
        EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
        if (!NoOpt && E.Plan.Port)
          EXPECT_NE(Native.find(std::string(E.Plan.LLVMName) + "(i16 " +
                                (E.Immediate ? "52" : "4660")),
                    std::string::npos)
              << Native;
      }
      if (A == Arch::X86) {
        const std::string Stem = "scalar32_" + std::to_string(ExportIndex++);
        saveX86SourceArtifact(
            Stem + ".bin",
            {reinterpret_cast<const char *>(E.Bytes.data()), E.Bytes.size()});
        saveX86SourceArtifact(Stem + ".high.c", freestandingC(High));
        saveX86SourceArtifact(Stem + ".llvm.c", freestandingC(LLVM));
      }
      for (const auto &Source : {High, LLVM}) {
        EXPECT_NE(Source.find(std::string(E.Plan.Builtin) + "("),
                  std::string::npos)
            << Source;
        compileNativeC(Compiler, Directory, GetParam().Format, Source,
                       E.Plan.LLVMName, Cases,
                       E.Plan.Port
                           ? std::optional<uint16_t>(E.Immediate ? 52 : 4660)
                           : std::nullopt,
                       A == Arch::X86, true, A);
        if (::testing::Test::HasFailure())
          return;
      }
    }
  EXPECT_EQ(Cases, 168u);
  RecordProperty("native_scalar_system_c_compilations", Cases);
}

TEST(X86ScalarSystemSource, NativeCallsRequireExactTypesAndEffects) {
  for (const auto &P : X86ScalarSystemNativeInventory) {
    const auto ID = llvm::Intrinsic::lookupIntrinsicID(P.LLVMName);
    if (!ID)
      GTEST_SKIP() << "requires the scalar MSR/port intrinsic SDK";
    SCOPED_TRACE(P.LLVMName);
    llvm::LLVMContext Context;
    llvm::Module Module("scalar_system", Context);
    auto *Callee = llvm::Intrinsic::getOrInsertDeclaration(&Module, ID);
    auto *Function = llvm::Function::Create(Callee->getFunctionType(),
                                            llvm::Function::ExternalLinkage,
                                            "scalar_system", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    std::vector<llvm::Value *> Inputs;
    for (auto &Argument : Function->args())
      Inputs.push_back(&Argument);
    auto *Call = Builder.CreateCall(Callee, Inputs);
    if (P.Write)
      Builder.CreateRetVoid();
    else
      Builder.CreateRet(Call);
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    const std::vector<std::string> Arguments(Inputs.size(), "input");
    auto Render = [&](Arch Target, bool Live = true,
                      const std::string &Result = "result") {
      return renderX86ScalarSystemCall(Target, BinaryFormat::ELF, *Call, Result,
                                       Live && !P.Write, Arguments);
    };
    for (auto A : {Arch::X86, Arch::X64}) {
      EXPECT_TRUE(Render(A));
      EXPECT_TRUE(Render(A, false, ""));
    }
    EXPECT_THROW((void)Render(Arch::AArch64), std::invalid_argument);
    for (const auto Effects :
         {llvm::MemoryEffects::none(), llvm::MemoryEffects::readOnly(),
          llvm::MemoryEffects::writeOnly(), llvm::MemoryEffects::argMemOnly(),
          llvm::MemoryEffects::inaccessibleMemOnly()}) {
      Call->setMemoryEffects(Effects);
      EXPECT_THROW((void)Render(Arch::X64), std::invalid_argument);
    }
    Call->setMemoryEffects(llvm::MemoryEffects::unknown());
    Call->setDoesNotReturn();
    EXPECT_THROW((void)Render(Arch::X64), std::invalid_argument);
    Call->removeFnAttr(llvm::Attribute::NoReturn);
    Call->setCallingConv(llvm::CallingConv::Fast);
    EXPECT_THROW((void)Render(Arch::X64), std::invalid_argument);
    Call->setCallingConv(llvm::CallingConv::C);
    if (!P.Write)
      EXPECT_THROW((void)Render(Arch::X64, true, ""), std::invalid_argument);
    EXPECT_THROW((void)renderX86ScalarSystemCall(Arch::X64, BinaryFormat::ELF,
                                                 *Call, "result", !P.Write, {}),
                 std::invalid_argument);
    Call->setArgOperand(0, Builder.getInt64(1));
    EXPECT_THROW((void)Render(Arch::X64), std::invalid_argument);
  }
}
