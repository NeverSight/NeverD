//===- X86NativeBitTestTests.inc - Native memory bit C tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject weakened contracts and compare projected native bit strings.
//===----------------------------------------------------------------------===//

TEST(LLVMCValues, X86BitStringIntrinsicsRejectWeakenedContracts) {
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    llvm::LLVMContext Context;
    llvm::Module Module("invalid-bit-string", Context);
    llvm::IRBuilder<> B(Context);
    const unsigned Bits = Mutation == 12 ? 64 : 32;
    llvm::Type *Pointer = Mutation == 2
                              ? static_cast<llvm::Type *>(B.getInt64Ty())
                              : B.getPtrTy(Mutation == 3 ? 256 : 0);
    auto *Native = llvm::Function::Create(
        llvm::FunctionType::get(
            Mutation == 0 ? B.getInt32Ty() : B.getInt8Ty(),
            {Pointer, B.getIntNTy(Mutation == 1 ? 16 : Bits)}, false),
        llvm::GlobalValue::ExternalLinkage,
        neverd::x86BitTestNativeName({Bits, "bts"}), Module);
    Native->addFnAttr(llvm::Attribute::NoUnwind);
    Native->addFnAttr(llvm::Attribute::Convergent);
    Native->addFnAttr(llvm::Attribute::NoDuplicate);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "invoke", Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Call = B.CreateCall(
        Native, {llvm::Constant::getNullValue(Pointer),
                 llvm::ConstantInt::get(
                     Native->getFunctionType()->getParamType(1), 0)});
    B.CreateRetVoid();
    if (Mutation == 4)
      Native->removeFnAttr(llvm::Attribute::Convergent);
    if (Mutation == 5)
      Native->removeFnAttr(llvm::Attribute::NoDuplicate);
    if (Mutation == 6)
      Native->removeFnAttr(llvm::Attribute::NoUnwind);
    if (Mutation == 7)
      Call->setMemoryEffects(llvm::MemoryEffects::none());
    if (Mutation == 8)
      Call->setMemoryEffects(llvm::MemoryEffects::writeOnly());
    if (Mutation == 9)
      Call->setMemoryEffects(llvm::MemoryEffects::argMemOnly());
    if (Mutation == 10)
      Call->addFnAttr(llvm::Attribute::WillReturn);
    if (Mutation == 13) {
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Native));
      B.CreateRet(B.getInt8(0));
    }
    const auto Target = Mutation == 11   ? neverd::Arch::AArch64
                        : Mutation == 12 ? neverd::Arch::X86
                                         : neverd::Arch::X64;
    EXPECT_DEATH((void)neverd::classifyX86BitTestNative(*Call, Target),
                 "invalid native x86 bit-string intrinsic contract");
  }
}

TEST(LLVMCValues, X86BitStringIntrinsicsMatchOriginalMemoryAndFaults) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires original Linux x64 bit-string instructions";
#else
  llvm::LLVMContext Context;
  llvm::Module Module("native-bit-strings", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-linux-gnu"));
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(Context);
  std::string Cases =
      "typedef unsigned char (*operation)(void *,uint64_t,void *);\n"
      "static const struct { const char *name; operation function; unsigned "
      "width,action,used; } cases[] = {\n";
  for (unsigned Bits : {16U, 32U, 64U}) {
    unsigned Action = 0;
    for (const char *Mnemonic : {"bt", "bts", "btr", "btc"}) {
      auto *Word = B.getIntNTy(Bits);
      auto *Native = llvm::Function::Create(
          llvm::FunctionType::get(B.getInt8Ty(), {B.getPtrTy(), Word}, false),
          llvm::GlobalValue::ExternalLinkage,
          neverd::x86BitTestNativeName({Bits, Mnemonic}), Module);
      Native->addFnAttr(llvm::Attribute::NoUnwind);
      Native->addFnAttr(llvm::Attribute::Convergent);
      Native->addFnAttr(llvm::Attribute::NoDuplicate);
      for (bool Used : {false, true}) {
        const std::string Name = std::string("probe_") + Mnemonic +
                                 std::to_string(Bits) + "_" +
                                 std::to_string(Used);
        auto *Function = llvm::Function::Create(
            llvm::FunctionType::get(
                B.getInt8Ty(), {B.getPtrTy(), B.getInt64Ty(), B.getPtrTy()},
                false),
            llvm::GlobalValue::ExternalLinkage, Name, Module);
        B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
        B.CreateStore(B.getInt8(17), Function->getArg(2));
        auto *CF = B.CreateCall(
            Native, {Function->getArg(0),
                     B.CreateTruncOrBitCast(Function->getArg(1), Word)});
        B.CreateStore(B.getInt8(29), Function->getArg(2));
        B.CreateRet(Used ? static_cast<llvm::Value *>(CF) : B.getInt8(0));
        Cases += "{\"" + Name + "\"," + Name + "," + std::to_string(Bits) +
                 "," + std::to_string(Action) + "," + std::to_string(Used) +
                 "},\n";
      }
      ++Action;
    }
  }
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  neverd::CEmitterOptions Options;
  Options.TheArch = neverd::Arch::X64;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  EXPECT_NE(Source.find("__builtin_ia32_bt_mem16"), std::string::npos);
  EXPECT_NE(Source.find("__builtin_ia32_btc_mem64"), std::string::npos);
  llvm::SmallString<256> Oracle(__FILE__);
  llvm::sys::path::remove_dots(Oracle, true);
  llvm::sys::path::remove_filename(Oracle);
  llvm::sys::path::append(Oracle, "X86NativeBitStringOracle.inc");
  Source += "\n" + Cases + "};\n#include \"" + Oracle.str().str() + "\"\n";
  for (const char *Level : {"-O0", "-O2"}) {
    compileAndRun(Source, Level, {}, true);
    if (::testing::Test::HasFailure())
      return;
    if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
      compileAndRun(Source, Level, {}, false, false, NeverC);
  }
#endif
}
